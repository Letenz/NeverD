//===- X64MachineProbe.cpp - Native startup validation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64MachineProbe.h"

#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::emulation {
namespace {
using namespace x64;
static_assert(probe::CodeOffset >= gateway::VectorCount * gateway::CodeStride);
static_assert(probe::CodeOffset + probe::programBytes() <= PageSize);
static_assert(probe::TLSOffset >= gateway::TSSOffset + gateway::TSSBytes);
static_assert(probe::TLSOffset >=
              gateway::GDTOffset + gateway::GDTEntries * WordBytes);
static_assert(probe::TLSOffset + 2 * WordBytes <= PageSize);
static_assert(probe::FloatingDestination < XmmCount &&
              probe::FloatingSource < XmmCount &&
              probe::VectorDestination < XmmCount &&
              probe::VectorSource < XmmCount);
} // namespace

llvm::Error verifyX64Machine(X64Machine &Machine, MemoryProjection &Memory) {
  if (auto E = Memory.beginRun())
    return E;
  auto Release = llvm::scope_exit([&] { Memory.endRun(); });
  // WHP does not need the exception gateway during ordinary execution, but
  // both native probes use the same private, supervisor-only storage.
  auto Root = buildX64PageTables(Memory, false, true);
  if (!Root)
    return Root.takeError();
  const auto Base = x64ExceptionMonitorBase(Memory);
  auto *Code = Memory.data() + x64::gateway::CodeGPA + x64::probe::CodeOffset;
  for (const auto &Instruction : x64::probe::Program) {
    std::memcpy(Code, Instruction.Code.data(), Instruction.Code.size());
    Code += Instruction.Code.size();
  }
  auto *TLS = Memory.data() + x64::gateway::DataGPA + x64::probe::TLSOffset;
  llvm::support::endian::write64le(TLS, x64::probe::FSValue);
  llvm::support::endian::write64le(TLS + x64::WordBytes, x64::probe::GSValue);
  X64MachineState State;
  for (unsigned Index = 0; Index < State.Registers.size(); ++Index)
    State.Registers[Index] =
        x64::probe::ScalarSeed + Index * x64::probe::ScalarStride;
  for (unsigned Index = 0; Index < State.Xmm.size(); ++Index)
    State.Xmm[Index] = {
        x64::probe::VectorLowSeed + Index * x64::probe::VectorStride,
        x64::probe::VectorHighSeed - Index * x64::probe::VectorStride};
  State.reg(X64Register::PC) = Base + x64::PageSize + x64::probe::CodeOffset;
  State.reg(X64Register::SP) =
      Base + x64::gateway::Bytes - x64::probe::StackAlignment;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::CR8) = x64::probe::CR8;
  State.reg(X64Register::CS) = x64::CodeSelector;
  State.reg(X64Register::SS) = x64::DataSelector;
  State.FSBase = Base + x64::probe::TLSOffset;
  State.GSBase = State.FSBase + x64::WordBytes;
  State.MXCSR = x64::probe::MXCSR;
  State.FP.Control = x64::probe::FPControl;
  State.FP.Status = x64::probe::FPStatus;
  State.FP.Tag = x64::probe::FPTag;
  State.FP.Opcode = x64::probe::FPOpcode;
  State.FP.Instruction = State.reg(X64Register::PC);
  State.FP.Data = State.FSBase;
  for (unsigned Index = 0; Index < State.FP.Registers.size(); ++Index)
    State.FP.Registers[Index] = {x64::probe::FP80Base + Index,
                                 x64::probe::FP80High};
  State.Xmm[x64::probe::FloatingDestination][0] =
      (State.Xmm[x64::probe::FloatingDestination][0] & ~x64::probe::FloatMask) |
      x64::probe::FloatOne;
  State.Xmm[x64::probe::FloatingSource][0] =
      (State.Xmm[x64::probe::FloatingSource][0] & ~x64::probe::FloatMask) |
      x64::probe::FloatHalfULP;
  const MachineRunControl Control{
      std::chrono::steady_clock::now() +
      std::chrono::microseconds(x64::probe::TimeoutMicroseconds)};
  for (const auto &Instruction : x64::probe::Program) {
    if (Control.interrupted())
      return diagnostic::interrupted(x64::probe::State, Control);
    auto Expected = State;
    Expected.reg(X64Register::PC) += Instruction.Code.size();
    switch (Instruction.Kind) {
    case x64::probe::Step::Nop:
      break;
    case x64::probe::Step::FloatingAdd:
      Expected.Xmm[x64::probe::FloatingDestination][0] =
          (Expected.Xmm[x64::probe::FloatingDestination][0] &
           ~x64::probe::FloatMask) |
          x64::probe::FloatRoundedUp;
      Expected.MXCSR = x64::probe::InexactMXCSR;
      break;
    case x64::probe::Step::VectorAdd:
      for (unsigned Word = 0; Word < RegisterValue{}.size(); ++Word)
        Expected.Xmm[x64::probe::VectorDestination][Word] +=
            Expected.Xmm[x64::probe::VectorSource][Word];
      break;
    case x64::probe::Step::ReadFS:
      Expected.reg(X64Register::R14) = x64::probe::FSValue;
      break;
    case x64::probe::Step::ReadGS:
      Expected.reg(X64Register::R15) = x64::probe::GSValue;
      break;
    case x64::probe::Step::ReadCS:
      Expected.reg(X64Register::R12) =
          (Expected.reg(X64Register::R12) & ~x64::probe::SelectorMask) |
          x64::CodeSelector;
      break;
    case x64::probe::Step::ReadSS:
      Expected.reg(X64Register::R13) =
          (Expected.reg(X64Register::R13) & ~x64::probe::SelectorMask) |
          x64::DataSelector;
      break;
    case x64::probe::Step::ReadCR8:
      Expected.reg(X64Register::R11) = x64::probe::CR8;
      break;
    }
    if (auto E = Machine.step(State, *Root, Control))
      return E;
    if (State != Expected)
      return diagnostic::error(x64::probe::State);
    if (Control.interrupted())
      return diagnostic::interrupted(x64::probe::State, Control);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
