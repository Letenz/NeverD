#include "neverd/loader/Swift/SwiftRuntimeCalls.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {

std::optional<SourceCallTypeHint>
swiftRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Bits != Bitness::Bits64 ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      Image.ConflictingImportStorageSlots.count(ImportSlot))
    return std::nullopt;
  const auto Import = Image.ImportPtrSlots.find(ImportSlot);
  if (Import == Image.ImportPtrSlots.end())
    return std::nullopt;
  llvm::StringRef Name(Import->second);
  if (!Name.consume_front("_"))
    return std::nullopt;

  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::SwiftRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Name.str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  // These entries have C_CC declarations in the Swift runtime ABI. In
  // particular, Direct refcount entries use SwiftDirectRR_CC and must not be
  // accepted by prefix matching. Keep calls and their memory effects intact.
  // https://github.com/swiftlang/swift/blob/main/include/swift/Runtime/RuntimeFunctions.def
  if (Name == "swift_retain" || Name == "swift_nonatomic_retain" ||
      Name == "swift_unknownObjectRetain" ||
      Name == "swift_nonatomic_unknownObjectRetain" ||
      Name == "swift_bridgeObjectRetain" ||
      Name == "swift_nonatomic_bridgeObjectRetain" ||
      Name == "swift_getObjectType") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"object", Pointer}};
  } else if (Name == "swift_release" || Name == "swift_nonatomic_release" ||
             Name == "swift_unknownObjectRelease" ||
             Name == "swift_nonatomic_unknownObjectRelease" ||
             Name == "swift_bridgeObjectRelease" ||
             Name == "swift_nonatomic_bridgeObjectRelease") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"object", Pointer}};
  } else if (Name == "swift_weakInit" || Name == "swift_weakAssign" ||
             Name == "swift_unknownObjectWeakInit" ||
             Name == "swift_unknownObjectWeakAssign") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}, {"object", Pointer}};
  } else if (Name == "swift_weakLoadStrong" || Name == "swift_weakTakeStrong" ||
             Name == "swift_unknownObjectWeakLoadStrong" ||
             Name == "swift_unknownObjectWeakTakeStrong") {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_weakDestroy" ||
             Name == "swift_unknownObjectWeakDestroy") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"reference", Pointer}};
  } else if (Name == "swift_beginAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"address", Pointer},
                            {"scratch", Pointer},
                            {"flags", NdType::makeInt(8, false)},
                            {"pc", Pointer}};
  } else if (Name == "swift_endAccess") {
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"scratch", Pointer}};
  } else {
    return std::nullopt;
  }
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

} // namespace neverd
