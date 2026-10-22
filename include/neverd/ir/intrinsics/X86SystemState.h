//===- X86SystemState.h - System instruction contracts -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Operand widths and completion for bounded x86 system instructions. Hidden
/// processor state remains owned by the instruction, including faults and
/// conditional descriptor accesses. This is not a software CPU-state model.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86SYSTEMSTATE_H
#define NEVERD_IR_INTRINSICS_X86SYSTEMSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

struct X86SystemStateShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  uint16_t OutputSize = 0;
  uint8_t Arguments = 0;
  std::array<uint16_t, 3> InputSizes{};
  std::array<bool, 3> ScalarInputs{};
};

struct X86SystemStateLayout {
  uint16_t OutputSize = 0;
  uint8_t Arguments = 0;
  std::array<uint16_t, 3> InputSizes{};
  // Two-output forms pack the value below a byte containing ZF.
  uint16_t ValueSize = 0;
  bool HasFlag = false;
};

inline std::optional<X86SystemStateLayout>
x86SystemStateLayout(Intrinsic Id, Arch TargetArch, uint16_t Width) {
  if (TargetArch != Arch::Unknown && TargetArch != Arch::X86 &&
      TargetArch != Arch::X64)
    return std::nullopt;
  const auto C = x86SystemStateContract(Id);
  if (!C.Mnemonic)
    return std::nullopt;
  using K = X86SystemStateKind;
  switch (C.Kind) {
  case K::ReadPid:
    if ((Width != 4 && Width != 8) || (TargetArch == Arch::X86 && Width != 4) ||
        (TargetArch == Arch::X64 && Width != 8))
      return std::nullopt;
    return X86SystemStateLayout{Width, 0, {}, Width};
  case K::ReadBase:
  case K::WriteBase:
    if (TargetArch == Arch::X86 || (Width != 4 && Width != 8))
      return std::nullopt;
    if (C.Kind == K::ReadBase)
      return X86SystemStateLayout{Width, 0, {}, Width};
    return X86SystemStateLayout{0, 1, {Width}};
  case K::Void:
    return X86SystemStateLayout{};
  case K::Monitor:
    // Addresses use LowIR's canonical 64-bit carrier on both architectures.
    return X86SystemStateLayout{0, 1, {8}};
  case K::Wait:
    return X86SystemStateLayout{1, 3, {4, 4, 4}, 0, true};
  case K::AccessRights:
    if ((Width != 2 && Width != 4 && Width != 8) ||
        (TargetArch == Arch::X86 && Width == 8))
      return std::nullopt;
    return X86SystemStateLayout{
        static_cast<uint16_t>(2 * Width),
        1,
        {C.MemoryArgument == 0 ? uint16_t{8} : uint16_t{2}},
        Width,
        true};
  case K::AdjustRpl:
    if (TargetArch == Arch::X64)
      return std::nullopt;
    if (C.MemoryArgument == 0)
      return X86SystemStateLayout{1, 2, {8, 2}, 0, true};
    return X86SystemStateLayout{4, 2, {2, 2}, 2, true};
  case K::ShadowWrite:
  case K::DirectStore:
    if ((Width != 4 && Width != 8) || (TargetArch == Arch::X86 && Width != 4))
      return std::nullopt;
    return X86SystemStateLayout{0, 2, {8, Width}};
  case K::Enqueue:
    // Non-long-mode ES bases and limits need an explicit segment contract.
    if (TargetArch == Arch::X86)
      return std::nullopt;
    return X86SystemStateLayout{1, 2, {8, 8}, 0, true};
  case K::DirectStore64B:
    if (TargetArch == Arch::X86)
      return std::nullopt;
    return X86SystemStateLayout{0, 2, {8, 8}};
  }
  return std::nullopt;
}

inline uint16_t x86SystemStateWidth(Intrinsic Id,
                                    const X86SystemStateShape &Shape) {
  switch (x86SystemStateContract(Id).Kind) {
  case X86SystemStateKind::ShadowWrite:
  case X86SystemStateKind::DirectStore:
    return Shape.InputSizes[1];
  case X86SystemStateKind::WriteBase:
    return Shape.InputSizes[0];
  case X86SystemStateKind::AccessRights:
    return Shape.OutputSize / 2;
  default:
    return Shape.OutputSize;
  }
}

inline uint16_t x86SystemStateMemoryBytes(Intrinsic Id, uint16_t Width) {
  switch (x86SystemStateContract(Id).Kind) {
  case X86SystemStateKind::Monitor:
    return 1;
  case X86SystemStateKind::ShadowWrite:
  case X86SystemStateKind::DirectStore:
    return Width;
  case X86SystemStateKind::Enqueue:
  case X86SystemStateKind::DirectStore64B:
    return 64;
  default:
    return 2;
  }
}

inline bool x86SystemStateShapeIsValid(Intrinsic Id,
                                       const X86SystemStateShape &Shape) {
  const auto C = x86SystemStateContract(Id);
  const auto L = x86SystemStateLayout(Id, Shape.TargetArch,
                                      x86SystemStateWidth(Id, Shape));
  if (!L || !Shape.ValidId || Shape.MemoryOrdering != NdMemoryOrdering::None ||
      !isKnownMemoryAddressSpace(Shape.MemoryAddressSpace) ||
      (C.MemoryArgument < 0 &&
       Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default) ||
      Shape.OutputSize != L->OutputSize ||
      (Shape.OutputSize && !Shape.WritableOutput) ||
      Shape.Arguments != L->Arguments)
    return false;
  for (uint8_t I = 0; I < L->Arguments; ++I)
    if (!Shape.ScalarInputs[I] || Shape.InputSizes[I] != L->InputSizes[I])
      return false;
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86SYSTEMSTATE_H
