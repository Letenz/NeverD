#include "neverd/loader/Swift/SwiftStringCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/ConvertUTF.h"

#include <algorithm>

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

std::optional<SwiftLiteralString> swiftLiteralString(const BinaryImage &Image,
                                                     va_t ImportSlot,
                                                     uint64_t CountAndFlags,
                                                     uint64_t Storage) {
  const auto Call = swiftStringSourceCallHint(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Call || Call->CallKind != SourceCallTypeHint::Kind::SwiftStringBridge ||
      Bind == Image.DyldBindSlots.end())
    return std::nullopt;
  const auto &Module = Bind->second.Module;
  if (Module != "/System/Library/Frameworks/Foundation.framework/Foundation" &&
      Module != "/System/Library/Frameworks/Foundation.framework/Versions/C/"
                "Foundation" &&
      Module != "/usr/lib/swift/libswiftFoundation.dylib")
    return std::nullopt;
  // Darwin's stable String representation stores a literal's UTF-8 address
  // with a bias and an immortal discriminator. Preserve the existing flags
  // and runtime bridge; reconstruct only the immutable byte storage.
  // https://github.com/swiftlang/swift/blob/main/stdlib/public/core/StringObject.swift
  constexpr uint64_t CountMask = UINT64_C(0x0000ffffffffffff);
  constexpr uint64_t LiteralFlags = UINT64_C(0x1000000000000000);
  constexpr uint64_t ASCIILiteralFlags = UINT64_C(0xd000000000000000);
  const uint64_t Flags = CountAndFlags & ~CountMask;
  const uint64_t Count = CountAndFlags & CountMask;
  if ((Flags != LiteralFlags && Flags != ASCIILiteralFlags) || !Count ||
      Count >= 1024 * 1024 ||
      (Storage & UINT64_C(0xf000000000000000)) !=
          SwiftLiteralString::ImmortalTag)
    return std::nullopt;
  const va_t Contents = (Storage & ~SwiftLiteralString::ImmortalTag) +
                        SwiftLiteralString::StorageBias;
  const auto Bytes = readImmutableImageBytes(Image, Contents, Count + 1);
  if (!Bytes || Bytes->back() != 0 ||
      std::find(Bytes->begin(), Bytes->end() - 1, 0) != Bytes->end() - 1)
    return std::nullopt;
  // One terminated extent gives each literal address one shared identity.
  // Embedded-zero literals and other flag combinations remain unsupported.
  const auto *Start = reinterpret_cast<const llvm::UTF8 *>(Bytes->data());
  if (!llvm::isLegalUTF8String(&Start, Start + Count) ||
      (Flags == ASCIILiteralFlags &&
       std::any_of(Bytes->begin(), Bytes->end(),
                   [](uint8_t Byte) { return Byte >= 0x80; })))
    return std::nullopt;
  return SwiftLiteralString{Contents, static_cast<uint32_t>(Count + 1)};
}
} // namespace neverd
