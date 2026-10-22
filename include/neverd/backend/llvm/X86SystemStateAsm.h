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
    P.InputBytes.push_back(isX86SystemStateAddressArgument(Id, I)
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
  case K::ShadowWrite:
    P.Text += Width == 8 ? "q " : "d ";
    P.Text += Input(1) + ", " + Memory(0);
    break;
  case K::Enqueue:
    P.Text += " " + Memory(0) + ", " + Input(1) + "\n\tsetz $0";
    break;
  case K::DirectStore:
    P.Text += " " + Input(1) + ", " + Memory(0);
    break;
  case K::DirectStore64B:
    P.Text += " " + Memory(0) + ", " + Input(1);
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

/// Fixed RAX data and RCX selector operands keep the documented ModRM roles.
/// LLVM 23's URDMSR register assembler reverses those roles. Exact bytes also
/// retain APX feature checks when an EVEX operand is allocated a low register.
inline std::optional<X86SystemStateAsm> x86MsrAccessAsm(X86MsrAccessKind Kind,
                                                        Arch TargetArch) {
  if (TargetArch != Arch::X64 || Kind >= X86MsrAccessKind::Count)
    return std::nullopt;
  using K = X86MsrAccessKind;
  const bool Write = x86MsrAccessIsWrite(Kind);
  const bool Immediate = x86MsrAccessHasImmediateSelector(Kind);
  const bool Privileged =
      Kind == K::RdmsrImmediate || Kind == K::WrmsrnsImmediate ||
      Kind == K::RdmsrVexImmediate || Kind == K::WrmsrnsVexImmediate;
  const bool Evex =
      Kind == K::RdmsrImmediate || Kind == K::WrmsrnsImmediate ||
      Kind == K::UrdmsrEvexRegister || Kind == K::UwrmsrEvexRegister ||
      Kind == K::UrdmsrEvexImmediate || Kind == K::UwrmsrEvexImmediate;
  X86SystemStateAsm P;
  P.Id = Intrinsic::X86MsrAccess;
  P.Text = "# x86 MSR: ";
  P.Text += Privileged ? (Write ? "wrmsrns" : "rdmsr")
                       : (Write ? "uwrmsr" : "urdmsr");
  P.Text += Evex ? " (EVEX)" : Immediate ? " (VEX)" : " (legacy)";
  P.Text += "\n\t.byte ";
  if (Evex) {
    P.Text += Immediate ? "0x62,0xf7," : "0x62,0xf4,";
    P.Text += Write ? "0x7e,0x08," : "0x7f,0x08,";
  } else if (Immediate) {
    P.Text += Write ? "0xc4,0xe7,0x7a," : "0xc4,0xe7,0x7b,";
  } else {
    P.Text += Write ? "0xf3,0x0f,0x38," : "0xf2,0x0f,0x38,";
  }
  P.Text += Privileged ? "0xf6," : "0xf8,";
  P.Text += Immediate ? "0xc0" : "0xc8";
  if (Immediate)
    P.Text += Write ? "\n\t.long ${0:c}" : "\n\t.long ${1:c}";
  P.InputConstraints = {Immediate ? "i" : "{cx}"};
  P.InputBytes = {static_cast<uint16_t>(Immediate ? 4 : 8)};
  if (Write) {
    P.InputConstraints.push_back("{ax}");
    P.InputBytes.push_back(8);
    P.Constraints = Immediate ? "i,{ax},~{memory}" : "{cx},{ax},~{memory}";
  } else {
    P.OutputConstraints = {"={ax}"};
    P.OutputBytes = {8};
    P.Constraints = Immediate ? "={ax},i,~{memory}" : "={ax},{cx},~{memory}";
  }
  return P;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86SYSTEMSTATEASM_H
