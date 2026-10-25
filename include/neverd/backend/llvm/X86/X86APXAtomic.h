//===- X86APXAtomic.h - Native APX atomic contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native API, encoding and assembly ownership for APX transactions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86APXATOMIC_H
#define NEVERD_BACKEND_LLVM_X86_X86APXATOMIC_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace neverd {

inline constexpr std::array<Intrinsic, 5> X86APXAtomicInventory = {
    Intrinsic::ApxRaoAdd, Intrinsic::ApxRaoAnd, Intrinsic::ApxRaoOr,
    Intrinsic::ApxRaoXor, Intrinsic::ApxCmpccXadd};
inline constexpr std::string_view X86APXAtomicAsmMarker =
    "# neverd.apx.atomic ";
inline constexpr unsigned X86APXConditionCount = 16;

struct X86APXAtomicPlan {
  Intrinsic Id;
  unsigned Bytes;
  unsigned AddressBits;
  unsigned SegmentPrefix;
  unsigned Condition;

  bool compare() const { return Id == Intrinsic::ApxCmpccXadd; }
  unsigned arguments() const { return compare() ? 3 : 2; }
  unsigned argumentBytes(unsigned I) const { return I ? Bytes : 8; }
  std::string operation() const {
    switch (Id) {
    case Intrinsic::ApxRaoAdd:
      return "aadd";
    case Intrinsic::ApxRaoAnd:
      return "aand";
    case Intrinsic::ApxRaoOr:
      return "aor";
    case Intrinsic::ApxRaoXor:
      return "axor";
    default:
      return "cmpccxadd";
    }
  }
  std::string name() const { return operation() + std::to_string(8 * Bytes); }
  std::string llvmName() const { return "llvm.x86." + name() + ".address"; }
  std::string builtin() const {
    return "__builtin_ia32_" + name() + "_address";
  }
  std::string mnemonic() const {
    constexpr const char *Conditions[] = {"o",  "no", "b",  "ae", "e", "ne",
                                          "be", "a",  "s",  "ns", "p", "np",
                                          "l",  "ge", "le", "g"};
    // GNU as infers RAO width from its register and rejects l/q suffixes.
    return compare() ? "cmp" + std::string(Conditions[Condition]) + "xadd"
                     : operation();
  }
  std::string segment() const {
    switch (SegmentPrefix) {
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
  std::string assembly() const {
    const auto Prefix = segment().empty() ? "" : "%" + segment() + ":";
    const auto Address = AddressBits == 32 ? (compare() ? "${1:k}" : "${0:k}")
                                           : (compare() ? "$1" : "$0");
    return std::string(X86APXAtomicAsmMarker) + name() + " " +
           std::to_string(AddressBits) + " " + std::to_string(SegmentPrefix) +
           " " + std::to_string(Condition) + "\n{evex} " + mnemonic() + " " +
           (compare() ? "$3, $0, " : "$1, ") + Prefix + "(" + Address + ")";
  }
  std::string constraints() const {
    return compare() ? "=r,{bx},0,r,~{flags},~{memory}" : "{bx},r,~{memory}";
  }
};

inline std::optional<X86APXAtomicPlan>
x86APXAtomicPlan(Intrinsic Id, unsigned Bytes, unsigned AddressBits,
                 unsigned SegmentPrefix, unsigned Condition = 0) {
  if (!isApxAtomicIntrinsic(Id) || (Bytes != 4 && Bytes != 8) ||
      (AddressBits != 32 && AddressBits != 64) ||
      Condition >= X86APXConditionCount ||
      (Id != Intrinsic::ApxCmpccXadd && Condition))
    return std::nullopt;
  switch (SegmentPrefix) {
  case 0:
  case 0x26:
  case 0x2e:
  case 0x36:
  case 0x3e:
  case 0x64:
  case 0x65:
    return X86APXAtomicPlan{Id, Bytes, AddressBits, SegmentPrefix, Condition};
  default:
    return std::nullopt;
  }
}

inline std::optional<unsigned> x86APXSegmentPrefix(NdMemoryAddressSpace Space) {
  switch (Space) {
  case NdMemoryAddressSpace::Default:
    return 0;
  case NdMemoryAddressSpace::X86FS:
    return 0x64;
  case NdMemoryAddressSpace::X86GS:
    return 0x65;
  }
  return std::nullopt;
}

inline std::optional<X86APXAtomicPlan> x86APXAtomicName(std::string_view Name) {
  for (const auto Id : X86APXAtomicInventory)
    for (unsigned Bytes : {4u, 8u}) {
      auto P = x86APXAtomicPlan(Id, Bytes, 64, 0);
      if (P->llvmName() == Name)
        return P;
    }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86APXATOMIC_H
