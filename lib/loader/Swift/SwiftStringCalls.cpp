#include "neverd/loader/Swift/SwiftStringCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd {
std::optional<SourceCallTypeHint>
swiftStringSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  if (!Import)
    return std::nullopt;
  const bool FromNSString =
      *Import == "_$sSS10FoundationE36_"
                 "unconditionallyBridgeFromObjectiveCySSSo8NSStringCSgFZ";
  if (!FromNSString &&
      *Import != "_$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF")
    return std::nullopt;
  SourceCallTypeHint Result;
  Result.CallKind = FromNSString
                        ? SourceCallTypeHint::Kind::SwiftStringFromNSString
                        : SourceCallTypeHint::Kind::SwiftStringBridge;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Import->str();
  auto &Signature = Result.Signature;
  Signature.Origin = SourceFunctionTypeHint::OriginKind::SwiftStringBridge;
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  if (FromNSString) {
    Signature.ReturnType = NdType::makeInt(16, false);
    Signature.Parameters = {{"object", Pointer}};
  } else {
    Signature.ReturnType = Pointer;
    Signature.Parameters = {{"word", NdType::makeInt(8, false)},
                            {"storage", Pointer}};
  }
  // These exact Darwin swiftcc entries carry ptr(i64, ptr) and {i64, ptr}(ptr),
  // without swiftself, swifterror or async context. Their integer register
  // locations agree with the ordinary Darwin layout on these two targets.
  // Keep separate call kinds so emitted declarations still use swiftcall.
  // The 128-bit carrier transports both String words without interpreting
  // tagged storage, claiming a source struct layout, or changing ownership.
  std::string Diagnostic;
  if (!assignDarwinScalarSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}
} // namespace neverd
