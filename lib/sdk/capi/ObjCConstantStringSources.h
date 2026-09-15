#ifndef NEVERD_SDK_CAPI_OBJCCONSTANTSTRINGSOURCES_H
#define NEVERD_SDK_CAPI_OBJCCONSTANTSTRINGSOURCES_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjC/ObjCConstantStrings.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringExtras.h"

#include <set>
#include <stdexcept>

namespace neverd::sdk {
inline std::optional<SourceCallTypeHint>
constantStringSourceHint(const BinaryImage &Image, va_t Address,
                         va_t PointerSlot = 0) {
  if ((PointerSlot &&
       readImmutableImagePointer(Image, PointerSlot) != Address) ||
      !readObjCConstantString(Image, Address))
    return std::nullopt;
  SourceCallTypeHint Hint;
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeConstantString;
  Hint.TargetAddress = Address;
  Hint.ImmutablePointerSlot = PointerSlot;
  Hint.Signature.ReturnType = NdType::makePtr(NdType::makeVoid());
  std::string Reason;
  if (!assignDarwinScalarSourceABI(Hint.Signature, Image.Arch, Reason))
    return std::nullopt;
  return Hint;
}

inline std::string
renderObjCConstantStringHelpers(const BinaryImage &Image,
                                const std::set<va_t> &Addresses,
                                std::set<std::string> &SharedFunctions) {
  std::string Source;
  for (va_t Address : Addresses) {
    const auto String = readObjCConstantString(Image, Address);
    if (!String)
      throw std::runtime_error(
          "constant-string source record is no longer valid");
    const auto Name = "neverd_objc_constant_string_" +
                      llvm::utohexstr(Address, true) + "_address";
    SharedFunctions.insert(Name);
    Source += "\nuintptr_t " + Name +
              "(void) {\n"
              "  extern int __CFConstantStringClassReference[];\n"
              "  static const " +
              std::string(String->UTF16 ? "uint16_t" : "unsigned char") +
              " units[] = { ";
    for (uint16_t Unit : String->Units)
      Source += std::to_string(Unit) + ", ";
    Source += "0 };\n"
              "  static struct { const void *isa; uint32_t flags; "
              "const void *units; int64_t length; } object = {\n"
              "    __CFConstantStringClassReference, " +
              std::string(String->UTF16 ? "0x7d0" : "0x7c8") + ", units, " +
              std::to_string(String->Units.size()) +
              "\n  };\n  return (uintptr_t)&object;\n}\n";
  }
  return Source;
}
} // namespace neverd::sdk
#endif
