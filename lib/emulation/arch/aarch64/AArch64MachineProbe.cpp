//===- AArch64MachineProbe.cpp - ARM64 startup probe -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64MachineProbe.h"

#include "../../core/ExecutionDiagnostics.h"
#include "AArch64Machine.h"

namespace neverd::emulation {
namespace {
namespace probe {
#define NEVERD_AARCH64_PROBE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "AArch64MachineProbe.def"
#undef NEVERD_AARCH64_PROBE_VALUE
} // namespace probe
} // namespace
llvm::Error verifyAArch64Machine(AArch64Machine &Machine,
                                 MemoryProjection &Memory) {
  if (auto E = buildAArch64PageTables(Memory))
    return E;
  AArch64MachineState State;
  for (unsigned Index = 0; Index < State.Registers.size(); ++Index)
    State.Registers[Index] = probe::ScalarSeed + Index * probe::ScalarStride;
  for (unsigned Index = 0; Index < State.Vectors.size(); ++Index)
    State.Vectors[Index] = {probe::VectorLowSeed + Index * probe::VectorStride,
                            probe::VectorHighSeed -
                                Index * probe::VectorStride};
  State.reg(AArch64Register::SP) =
      aarch64::EntryGPA + memory::PageSize - probe::StackAlignment;
  State.reg(AArch64Register::PC) = aarch64::ProbePC;
  State.reg(AArch64Register::NZCV) = probe::NZCV;
  State.reg(AArch64Register::FPCR) = probe::FPCR;
  State.reg(AArch64Register::FPSR) = probe::FPSR;
  State.Vectors[probe::FirstVector][0] = probe::FloatHalfULP;
  State.Vectors[probe::LastVector][0] = probe::FloatOne;
  const auto Deadline =
      std::chrono::steady_clock::now() +
      std::chrono::microseconds(aarch64::ProbeTimeoutMicroseconds);
  const MachineRunControl Control{Deadline};
  for (const auto &Instruction : aarch64::probe::Program) {
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::ArmState, Control);
    auto Expected = State;
    Expected.reg(AArch64Register::PC) += aarch64::InstructionBytes;
    switch (Instruction.Kind) {
    case aarch64::probe::Step::Nop:
      break;
    case aarch64::probe::Step::FloatingAdd:
      Expected.Vectors[probe::ResultVector] = {probe::FloatRoundedUp, 0};
      Expected.reg(AArch64Register::FPSR) = probe::InexactFPSR;
      break;
    case aarch64::probe::Step::VectorAdd:
      Expected.Vectors[probe::LastVector] = {probe::VectorResultLow,
                                             probe::VectorResultHigh};
      break;
    }
    if (auto E = Machine.step(State, Control))
      return E;
    if (State.UserMode != Expected.UserMode ||
        State.Registers != Expected.Registers ||
        State.Vectors != Expected.Vectors)
      return diagnostic::error(diagnostic::ArmState);
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::ArmState, Control);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
