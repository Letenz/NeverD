//===- X86SystemStateAsm.h - Owned system assembly contracts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Assembly operands shared by LLVM and both C routes. Architectural memory
/// operations stay inside the instruction; every event remains observable.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86SYSTEMSTATEASM_H
#define NEVERD_BACKEND_LLVM_X86SYSTEMSTATEASM_H

#include "neverd/ir/intrinsics/X86SystemState.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct X86SystemStateAsm {
  Intrinsic Id = Intrinsic::None;
  std::string Text;
  std::string Constraints;
  std::vector<std::string> OutputConstraints;
  std::vector<std::string> InputConstraints;
  std::vector<uint16_t> OutputBytes;
  std::vector<uint16_t> InputBytes;
  bool ChangesFlags = false;
};

inline std::optional<X86SystemStateAsm>
x86SystemStateAsm(Intrinsic Id, Arch TargetArch, uint16_t Width,
                  NdMemoryAddressSpace Space) {
  const auto C = x86SystemStateContract(Id);
  const auto L = x86SystemStateLayout(Id, TargetArch, Width);
  if (!L || (TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      !isKnownMemoryAddressSpace(Space) ||
      (C.MemoryArgument < 0 && Space != NdMemoryAddressSpace::Default))
    return std::nullopt;
  X86SystemStateAsm P;
  P.Id = Id;
  P.Text = C.Mnemonic;
  P.ChangesFlags = L->HasFlag;
  if (L->ValueSize) {
    P.OutputConstraints.push_back("=&r");
    P.OutputBytes.push_back(L->ValueSize);
  }
  if (L->HasFlag) {
    P.OutputConstraints.push_back("=q");
    P.OutputBytes.push_back(1);
  }
  for (uint8_t I = 0; I < L->Arguments; ++I) {
    P.InputConstraints.push_back("r");
    P.InputBytes.push_back(I == C.MemoryArgument
                               ? (TargetArch == Arch::X64 ? 8 : 4)
                               : L->InputSizes[I]);
  }
  const std::string Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                              : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                                     : "";
  auto Input = [&](unsigned I) {
    return "$" + std::to_string(P.OutputBytes.size() + I);
  };
  auto Memory = [&](unsigned I) { return Segment + "(" + Input(I) + ")"; };
  using K = X86SystemStateKind;
  switch (C.Kind) {
  case K::ReadPid:
  case K::ReadBase:
    P.Text += " $0";
    break;
  case K::WriteBase:
    P.Text += " " + Input(0);
    break;
  case K::Void:
    break;
  case K::Monitor:
    if (!Segment.empty())
      P.Text = (Space == NdMemoryAddressSpace::X86FS ? "fs " : "gs ") + P.Text;
    P.Text += " " + Input(0);
    break;
  case K::Wait:
    P.InputConstraints = {"r", "{ax}", "{dx}"};
    P.Text += " " + Input(0) + "\n\tsetc $0";
    break;
  case K::AccessRights:
    P.Text += " " + (C.MemoryArgument == 0 ? Memory(0) : Input(0)) +
              ", $0\n\tsetz $1";
    break;
  case K::AdjustRpl:
    if (C.MemoryArgument == 0)
      P.Text += " " + Input(1) + ", " + Memory(0) + "\n\tsetz $0";
    else {
      P.InputConstraints[0] = "0";
      P.Text += " " + Input(1) + ", $0\n\tsetz $1";
    }
    break;
  }
  auto Constraint = [&](const std::string &Text) {
    if (!P.Constraints.empty())
      P.Constraints += ',';
    P.Constraints += Text;
  };
  for (const auto &O : P.OutputConstraints)
    Constraint(O);
  for (const auto &I : P.InputConstraints)
    Constraint(I);
  if (P.ChangesFlags)
    Constraint("~{cc}");
  Constraint("~{memory}");
  return P;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86SYSTEMSTATEASM_H
