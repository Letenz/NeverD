#include "neverd/loader/Swift/SwiftStringCalls.h"

#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <array>

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
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (Bind == Image.DyldBindSlots.end() ||
      !darwinExportModuleMatches(
          "/System/Library/Frameworks/Foundation.framework/Foundation|"
          "/System/Library/Frameworks/Foundation.framework/Versions/C/"
          "Foundation|/usr/lib/swift/libswiftFoundation.dylib",
          Bind->second.Module))
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
    Result.SwiftStringInputs = {{0, 1}};
  }
  // These exact Darwin swiftcc entries carry ptr(i64, ptr) and {i64, ptr}(ptr),
  // without swiftself, swifterror or async context. Their integer register
  // locations agree with the ordinary Darwin layout on these two targets.
  // Keep separate call kinds so emitted declarations still use swiftcall.
  // The 128-bit carrier transports both String words without interpreting
  // tagged storage, claiming a source struct layout, or changing ownership.
  std::string Diagnostic;
  if (!assignDarwinSwiftSourceABI(Signature, Image.Arch, Diagnostic))
    return std::nullopt;
  return Result;
}

std::optional<SwiftLiteralString> swiftLiteralString(const BinaryImage &Image,
                                                     uint64_t CountAndFlags,
                                                     uint64_t Storage) {
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

bool isCanonicalSwiftSmallString(uint64_t Payload, uint64_t TaggedPayload) {
  // StringObject.swift and SmallString.swift define the discriminator, UTF-8
  // byte count, exact ASCII flag, and zero padding for this value-only form.
  const uint8_t Marker = TaggedPayload >> 56;
  if ((Marker & 0xf0) != 0xa0 && (Marker & 0xf0) != 0xe0)
    return false;
  const unsigned Count = Marker & 0x0f;
  std::array<llvm::UTF8, 15> Bytes{};
  bool ASCII = true;
  for (unsigned I = 0; I < Bytes.size(); ++I) {
    Bytes[I] = I < 8 ? Payload >> (I * 8) : TaggedPayload >> ((I - 8) * 8);
    if (I >= Count && Bytes[I])
      return false;
    ASCII &= Bytes[I] < 0x80;
  }
  const auto *Start = Bytes.data();
  return ASCII == bool(Marker & 0x40) &&
         llvm::isLegalUTF8String(&Start, Start + Count);
}

std::optional<SwiftLiteralString>
swiftStaticStringLiteral(const BinaryImage &Image, uint64_t Data,
                         uint64_t ByteCount, uint8_t Flags) {
  // StaticString.swift stores either an immutable UTF-8 pointer (low bit 0)
  // or a Unicode scalar (low bit 1); bit 1 records known ASCII contents.
  // https://github.com/swiftlang/swift/blob/swift-6.1.2-RELEASE/stdlib/public/core/StaticString.swift
  if ((Flags != 0 && Flags != 2) || ByteCount > 1024 * 1024)
    return std::nullopt;
  const auto Bytes = readImmutableImageBytes(Image, Data, ByteCount);
  if (!Bytes ||
      (Flags == 2 && std::any_of(Bytes->begin(), Bytes->end(),
                                 [](uint8_t Byte) { return Byte >= 0x80; })))
    return std::nullopt;
  if (!Bytes->empty()) {
    const auto *Start = reinterpret_cast<const llvm::UTF8 *>(Bytes->data());
    if (!llvm::isLegalUTF8String(&Start, Start + Bytes->size()))
      return std::nullopt;
  }
  return SwiftLiteralString{Data, static_cast<uint32_t>(ByteCount)};
}

bool isCanonicalSwiftStaticStringScalar(uint64_t Data, uint64_t ByteCount,
                                        uint8_t Flags) {
  return !ByteCount && Data <= 0x10ffff &&
         !(Data >= 0xd800 && Data <= 0xdfff) && Flags == (Data < 0x80 ? 3 : 1);
}
} // namespace neverd
