//===- X64FPState.cpp - Architectural x87 state and FXSAVE64 layout
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64FPState.h"

#include "../../core/ExecutionDiagnostics.h"
#include "X64Machine.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cassert>

namespace neverd::emulation {
using namespace x64::fp;
uint16_t X64FPState::fullTag() const {
  uint16_t Full = 0;
  for (unsigned I = 0; I < Registers.size(); ++I) {
    unsigned Kind = EmptyTag;
    if (Tag & (1u << I)) {
      const auto &V = Registers[I];
      const auto Exponent = V[1] & ExponentMask;
      if (!Exponent && !V[0])
        Kind = ZeroTag;
      else if (!Exponent || Exponent == ExponentMask || !(V[0] & IntegerBit))
        Kind = SpecialTag;
      else
        Kind = ValidTag;
    }
    Full |= Kind << (I * TagBits);
  }
  return Full;
}
void X64FPState::setFullTag(uint16_t Full) {
  Tag = 0;
  for (unsigned I = 0; I < Registers.size(); ++I)
    if (((Full >> (I * TagBits)) & EmptyTag) != EmptyTag)
      Tag |= 1u << I;
}
bool isX64FPRegister(CPURegister R) {
  return R >= CPURegister::X64FPCW && R <= CPURegister::X64FP7;
}
llvm::Error validateX64FPRegister(CPURegister R, const RegisterValue &V) {
  if (!isX64FPRegister(R) || !registerValueFits(R, V))
    return diagnostic::error(diagnostic::Register);
  if (R == CPURegister::X64FPCW &&
      ((V[0] & ~AllowedControl) || !(V[0] & ReservedControlBit) ||
       (V[0] & PrecisionMask) == ReservedPrecision))
    return diagnostic::error(diagnostic::Register);
  if (R == CPURegister::X64FPOP && (V[0] & ~OpcodeMask))
    return diagnostic::error(diagnostic::Register);
  return llvm::Error::success();
}
RegisterValue readX64FPRegister(const X64FPState &State, CPURegister R) {
  switch (R) {
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  case CPURegister::X64##Name:                                                 \
    return {State.Member, 0};
#include "X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  case CPURegister::X64FPTag:
    return {State.Tag, 0};
  default:
    assert(R >= CPURegister::X64FP0 && R <= CPURegister::X64FP7);
    return State.Registers[unsigned(R) - unsigned(CPURegister::X64FP0)];
  }
}
llvm::Error writeX64FPRegister(X64FPState &State, CPURegister R,
                               const RegisterValue &V) {
  if (auto E = validateX64FPRegister(R, V))
    return E;
  switch (R) {
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  case CPURegister::X64##Name:                                                 \
    State.Member = V[0];                                                       \
    break;
#include "X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  case CPURegister::X64FPTag:
    State.Tag = V[0];
    break;
  default:
    State.Registers[unsigned(R) - unsigned(CPURegister::X64FP0)] = V;
    break;
  }
  return llvm::Error::success();
}
llvm::Error validateX64FPState(const X64FPState &State) {
  if (auto E = validateX64FPRegister(CPURegister::X64FPCW, {State.Control, 0}))
    return E;
  if (auto E = validateX64FPRegister(CPURegister::X64FPOP, {State.Opcode, 0}))
    return E;
  for (const auto &V : State.Registers)
    if (!registerValueFits(CPURegister::X64FP0, V))
      return diagnostic::error(diagnostic::Register);
  return llvm::Error::success();
}
llvm::Error encodeX64FXState(const X64MachineState &State,
                             llvm::MutableArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < LegacyBytes)
    return diagnostic::error(diagnostic::FPState);
  if (auto E = validateX64FPState(State.FP))
    return E;
  std::fill(Bytes.begin(), Bytes.begin() + LegacyBytes, 0);
  auto *P = Bytes.data();
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  llvm::support::endian::write<Type, llvm::endianness::little>(                \
      P + Offset, State.FP.Member);
#include "X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  P[TagOffset] = State.FP.Tag;
  llvm::support::endian::write32le(P + MXCSROffset, State.MXCSR);
  llvm::support::endian::write32le(P + MXCSRMaskOffset, x64::AllowedMXCSR);
  for (unsigned I = 0; I < State.FP.Registers.size(); ++I) {
    const auto &V = State.FP.Registers[(State.FP.top() + I) % RegisterCount];
    auto *Slot = P + RegistersOffset + I * RegisterSlotBytes;
    llvm::support::endian::write64le(Slot, V[0]);
    llvm::support::endian::write16le(Slot + sizeof(V[0]), V[1]);
  }
  for (unsigned I = 0; I < State.Xmm.size(); ++I) {
    auto *Slot = P + XmmOffset + I * x64::VectorBytes;
    llvm::support::endian::write64le(Slot, State.Xmm[I][0]);
    llvm::support::endian::write64le(Slot + sizeof(uint64_t), State.Xmm[I][1]);
  }
  return llvm::Error::success();
}
llvm::Error decodeX64FXState(X64MachineState &State,
                             llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < LegacyBytes)
    return diagnostic::error(diagnostic::FPState);
  const auto *P = Bytes.data();
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  State.FP.Member =                                                            \
      llvm::support::endian::read<Type, llvm::endianness::little>(P + Offset);
#include "X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
  State.FP.Tag = P[TagOffset];
  State.MXCSR = llvm::support::endian::read32le(P + MXCSROffset);
  for (unsigned I = 0; I < State.FP.Registers.size(); ++I) {
    auto &V = State.FP.Registers[(State.FP.top() + I) % RegisterCount];
    const auto *Slot = P + RegistersOffset + I * RegisterSlotBytes;
    V = {llvm::support::endian::read64le(Slot),
         llvm::support::endian::read16le(Slot + sizeof(V[0]))};
  }
  for (unsigned I = 0; I < State.Xmm.size(); ++I) {
    const auto *Slot = P + XmmOffset + I * x64::VectorBytes;
    State.Xmm[I] = {llvm::support::endian::read64le(Slot),
                    llvm::support::endian::read64le(Slot + sizeof(uint64_t))};
  }
  return llvm::Error::success();
}
llvm::Error encodeX64XsaveState(const X64MachineState &State,
                                llvm::MutableArrayRef<uint8_t> Bytes,
                                bool Compact) {
  if (Bytes.size() < XsaveBytes || Bytes.size() > MaxXsaveBytes ||
      (State.MXCSR & ~x64::AllowedMXCSR))
    return diagnostic::error(diagnostic::FPState);
  if (auto E = encodeX64FXState(State, Bytes))
    return E;
  std::fill(Bytes.begin() + LegacyBytes, Bytes.end(), 0);
  llvm::support::endian::write64le(Bytes.data() + XStateOffset, FPAndSSE);
  llvm::support::endian::write64le(Bytes.data() + XCompOffset,
                                   Compact ? Compacted | FPAndSSE : 0);
  return llvm::Error::success();
}
llvm::Error decodeX64XsaveState(X64MachineState &State,
                                llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() < XsaveBytes || Bytes.size() > MaxXsaveBytes)
    return diagnostic::error(diagnostic::FPState);
  const auto Present =
      llvm::support::endian::read64le(Bytes.data() + XStateOffset);
  const auto Layout =
      llvm::support::endian::read64le(Bytes.data() + XCompOffset);
  if ((Present & ~FPAndSSE) || (Layout && !(Layout & Compacted)) ||
      ((Layout & Compacted) && (Present & ~Layout)))
    return diagnostic::error(diagnostic::FPState);
  for (const auto Byte :
       Bytes.slice(XCompOffset + sizeof(uint64_t),
                   XsaveBytes - XCompOffset - sizeof(uint64_t)))
    if (Byte)
      return diagnostic::error(diagnostic::FPState);
  auto Next = State;
  if (auto E = decodeX64FXState(Next, Bytes))
    return E;
  // Architecturally absent components contain init state, independently of
  // stale legacy bytes. Only active fields participate in validation.
  if (!(Present & X87Present))
    Next.FP = {};
  if (!(Present & SSEPresent)) {
    // Standard XRSTOR reads MXCSR independently of XSTATE_BV[1]. Only
    // compacted init state resets it along with the XMM registers.
    if (Layout & Compacted)
      Next.MXCSR = x64::InitialMXCSR;
    Next.Xmm = {};
  }
  if (Next.MXCSR & ~x64::AllowedMXCSR)
    return diagnostic::error(diagnostic::FPState);
  if (auto E = validateX64FPState(Next.FP))
    return E;
  State = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
