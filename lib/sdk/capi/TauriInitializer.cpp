//===- TauriInitializer.cpp - Immutable native initializer join ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native loader and LowIR are authoritative for boundaries and field roles.
/// The web reader alone owns PHF, paths, compression and extracted namespaces.
///
//===----------------------------------------------------------------------===//

#include "TauriInitializer.h"

#include "neverd/pipeline/ConstantStores.h"
#include "neverd/sdk/Limits.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/web/Error.h"

#include "llvm/ADT/StringExtras.h"

#include <cstring>

namespace neverd::sdk {
web::TauriInitializer tauriInitializer(const web::NativeInput &Input,
                                       uint64_t Start) {
  if (Input.Content.size() > web::MaxTauriInputBytes)
    throw web::Error("tauri_input_budget_exceeded");
  auto Bytes = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(
      Input.Content.size(), "<tauri-native>");
  if (!Bytes)
    throw web::Error("tauri_allocation_failed");
  for (uint64_t At = 0; At < Input.Content.size();) {
    const auto Part = Input.Content.read(
        At, std::min(web::BlobTransferBytes, Input.Content.size() - At));
    std::memcpy(Bytes->getBufferStart() + At, Part.data(), Part.size());
    At += Part.size();
  }
  auto Image = loadBinaryBuffer(Bytes->getMemBufferRef());
  if (!Image) {
    llvm::consumeError(Image.takeError());
    throw web::Error("tauri_native_load_failed");
  }
  if (Image->Format != BinaryFormat::MachO || Image->Arch != Arch::X64 ||
      !Image->is64Bit() || Image->IsRelocatable ||
      Image->MachOHasChainedFixups || Image->MachOChainedFixupsAmbiguous)
    throw web::Error("tauri_native_profile_unavailable");
  if (!Image->InputFileSHA256 ||
      llvm::toHex(*Image->InputFileSHA256, true) != Input.BlobHash)
    throw web::Error("tauri_native_input_hash_mismatch");
  auto Stores = projectNativeConstantStores(*Image, Start, 5);
  if (!Stores) {
    const auto Code = llvm::toString(Stores.takeError());
    throw web::Error(Code.c_str());
  }
  web::TauriInitializer Out;
  Out.InputHash = Input.BlobHash;
  Out.Start = Stores->Start;
  Out.End = Stores->End;
  Out.Function = Stores->FunctionEntry;
  Out.Instructions = Stores->Instructions;
  Out.NativeSteps = Stores->Steps;
  Out.Offset = Image->vaToFileOffset(Start);
  for (size_t I = 0; I < Out.Fields.size(); ++I) {
    const auto &S = Stores->Stores.at(I);
    if (isAddressProvenance(S.Provenance) &&
        !isExactAddressProvenance(S.Provenance))
      throw web::Error("tauri_native_pointer_role_unavailable");
    Out.Fields[I] = {S.Instruction, S.Value,
                     isExactAddressProvenance(S.Provenance)};
  }
  if (Image->Segments.size() >
      ::neverd::sdk::kTauriInitializerImageSegmentsSizeLimit)
    throw web::Error("tauri_mapping_budget_exceeded");
  for (const auto &S : Image->Segments)
    if (S.FileSz)
      Out.Mappings.push_back(
          {S.VA, S.Size, S.FileOff, S.FileSz, S.isReadable()});
  return Out;
}
} // namespace neverd::sdk
