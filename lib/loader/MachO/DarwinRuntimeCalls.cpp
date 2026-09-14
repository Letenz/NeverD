#include "neverd/loader/MachO/DarwinRuntimeCalls.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(ImportSlot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(ImportSlot);
  if (Import == Image.ImportPtrSlots.end())
    return std::nullopt;
  if (auto I = Image.ImportStorageSlots.find(ImportSlot);
      I != Image.ImportStorageSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend))
    return std::nullopt;
  if (auto I = Image.DyldBindSlots.find(ImportSlot);
      I != Image.DyldBindSlots.end() &&
      (I->second.Name != Import->second || I->second.Addend ||
       I->second.WeakImport))
    return std::nullopt;
  llvm::StringRef Name(Import->second);
  if (!Name.consume_front("_"))
    return std::nullopt;

  // The public lock routines use one pointer to stable, opaque lock storage.
  // Call the real platform implementation, including its ownership checks.
  // https://github.com/apple-oss-distributions/libplatform/blob/main/include/os/lock.h
  const bool TryLock = Name == "os_unfair_lock_trylock";
  if (!TryLock && Name != "os_unfair_lock_lock" &&
      Name != "os_unfair_lock_unlock" &&
      Name != "os_unfair_lock_assert_owner" &&
      Name != "os_unfair_lock_assert_not_owner")
    return std::nullopt;
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Signature.ReturnType =
      TryLock ? NdType::makeInt(1, false) : NdType::makeVoid();
  Signature.Parameters = {{"lock", NdType::makePtr(NdType::makeVoid())}};
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}
} // namespace neverd
