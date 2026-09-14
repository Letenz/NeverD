#include "neverd/loader/Swift/SwiftStringCalls.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd {
std::optional<SourceCallTypeHint>
swiftStringSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(ImportSlot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(ImportSlot);
  if (Import == Image.ImportPtrSlots.end() ||
      Import->second != "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF")
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

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::SwiftStringBridge;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Import->second;
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftStringBridge;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Signature.ReturnType = Pointer;
  Signature.Parameters = {{"word", NdType::makeInt(8, false)},
                          {"storage", Pointer}};
  // This specific Darwin swiftcc entry is ptr(i64, ptr), without swiftself,
  // swifterror or async context. Its scalar registers and preserved registers
  // agree with the ordinary Darwin scalar layout on these two targets. Keep
  // its separate call kind so the source emitter still declares swiftcall.
  // Storage may contain tagged bits; neither word is decoded or dereferenced.
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}
} // namespace neverd
