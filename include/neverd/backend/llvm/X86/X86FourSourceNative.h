//===- X86FourSourceNative.h - Native four-source transport ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native and owned assembly interfaces retain the entire source register
/// group.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_X86FOURSOURCENATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86FOURSOURCENATIVE_H

#include "neverd/ir/intrinsics/X86/X86FourSource.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd {
inline constexpr std::string_view X86FourSourceAsmMarker =
    "# neverd.x86.four_source ";
struct X86FourSourcePlan {
  X86FourSourceControl Control;
  unsigned Prefix;
  std::string llvmName() const {
    return std::string("llvm.x86.avx512.") + Control.mnemonic() + ".native";
  }
  std::string builtin() const {
    return std::string("__builtin_ia32_") + Control.mnemonic() + "_native";
  }
  std::vector<std::string> features() const {
    return Control.scalar() ? std::vector<std::string>{"avx512f"}
                            : std::vector<std::string>{"avx512f", "evex512"};
  }
  const char *segment() const {
    switch (Prefix) {
    case 0x26:
      return "es";
    case 0x2e:
      return "cs";
    case 0x36:
      return "ss";
    case 0x3e:
      return "ds";
    case 0x64:
      return "fs";
    case 0x65:
      return "gs";
    default:
      return "";
    }
  }
  std::string sourceRegister(unsigned I) const {
    return std::string(Control.scalar() ? "xmm" : "zmm") +
           std::to_string(X86FourSourceCount + I);
  }
  std::string assembly() const {
    return std::string(X86FourSourceAsmMarker) +
           std::to_string(static_cast<unsigned>(Control.Kind)) + " " +
           std::to_string(Control.Zero) + " " + std::to_string(Prefix) + "\n" +
           Control.mnemonic() + " " +
           (Prefix ? std::string("%") + segment() + ":" : "") +
           "($1), $3, $0 {$7}" + (Control.Zero ? " {z}" : "");
  }
  // AX cannot be a reserved frame, realignment or PIC base. Keep the old BX
  // spelling readable for owned IR emitted before this carrier correction.
  std::string constraints(bool LegacyBX = false) const {
    std::string R = LegacyBX ? "=v,{bx},0" : "=v,{ax},0";
    for (unsigned I = 0; I < X86FourSourceCount; ++I)
      R += ",{" + sourceRegister(I) + "}";
    return R + ",^Yk,~{memory}";
  }
};
inline std::optional<X86FourSourcePlan>
x86FourSourcePlan(unsigned Kind, unsigned Zero, unsigned Prefix) {
  if (Kind > static_cast<unsigned>(X86FourSourceKind::DPWSSDS) || Zero > 1 ||
      (Prefix && Prefix != 0x26 && Prefix != 0x2e && Prefix != 0x36 &&
       Prefix != 0x3e && Prefix != 0x64 && Prefix != 0x65))
    return {};
  return X86FourSourcePlan{{static_cast<X86FourSourceKind>(Kind), bool(Zero)},
                           Prefix};
}
inline std::optional<X86FourSourcePlan>
x86FourSourceNativeName(std::string_view N) {
  for (unsigned I = 0; I <= static_cast<unsigned>(X86FourSourceKind::DPWSSDS);
       ++I) {
    const auto P = *x86FourSourcePlan(I, 0, 0);
    if (P.llvmName() == N)
      return P;
  }
  return {};
}
} // namespace neverd
#endif // NEVERD_BACKEND_LLVM_X86_X86FOURSOURCENATIVE_H
