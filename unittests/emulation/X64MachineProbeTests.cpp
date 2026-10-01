//===- X64MachineProbeTests.cpp - Complete startup state tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64MachineProbe.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::emulation {
namespace {
#define NEVERD_X64_PROBE_TEST_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#include "X64ProbeCases.def"
#undef NEVERD_X64_PROBE_TEST_VALUE
using Corruption = void (*)(X64MachineState &);
class CorruptingTransport final : public X64Machine {
public:
  unsigned Scalar = 0, Vector = 0, Word = 0, Entries = 0;
  bool CorruptVector = false, CorruptFP = false, NopOnly = false;
  bool FailTransfer = false;
  Corruption Corrupt = nullptr;
  llvm::Error step(X64MachineState &State, uint64_t,
                   MachineRunControl) override {
    const auto Index = Entries++;
    if (FailTransfer)
      return diagnostic::error(diagnostic::KvmState);
    State.reg(X64Register::PC) += x64::probe::Program[Index].Code.size();
    if (Corrupt)
      Corrupt(State);
    else if (NopOnly)
      return llvm::Error::success();
    else if (CorruptVector)
      State.Xmm[Vector][Word] ^= CorruptBit;
    else if (CorruptFP)
      State.FP.Registers[Vector][Word] ^= CorruptBit;
    else
      State.Registers[Scalar] ^= CorruptBit;
    return llvm::Error::success();
  }
};
TEST(X64MachineProbe, EveryScalarCorruptionRejectsNativeInitialization) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  X64MachineState Inventory;
  for (unsigned Index = 0; Index < Inventory.Registers.size(); ++Index) {
    SCOPED_TRACE(Index);
    CorruptingTransport Machine;
    Machine.Scalar = Index;
    EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
              x64::probe::State);
    EXPECT_EQ(Machine.Entries, 1u);
  }
}
TEST(X64MachineProbe, EveryXmmWordCorruptionRejectsNativeInitialization) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  X64MachineState Inventory;
  for (unsigned Index = 0; Index < Inventory.Xmm.size(); ++Index)
    for (unsigned Word = 0; Word < RegisterValue{}.size(); ++Word) {
      SCOPED_TRACE(Index);
      SCOPED_TRACE(Word);
      CorruptingTransport Machine;
      Machine.CorruptVector = true;
      Machine.Vector = Index;
      Machine.Word = Word;
      EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
                x64::probe::State);
      EXPECT_EQ(Machine.Entries, 1u);
    }
}
TEST(X64MachineProbe, EveryPhysicalFPWordCorruptionRejectsInitialization) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  X64MachineState Inventory;
  for (unsigned Index = 0; Index < Inventory.FP.Registers.size(); ++Index)
    for (unsigned Word = 0; Word < RegisterValue{}.size(); ++Word) {
      SCOPED_TRACE(Index);
      SCOPED_TRACE(Word);
      CorruptingTransport Machine;
      Machine.CorruptFP = true;
      Machine.Vector = Index;
      Machine.Word = Word;
      EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
                x64::probe::State);
      EXPECT_EQ(Machine.Entries, 1u);
    }
}
TEST(X64MachineProbe, ControlTLSAndPrivilegeCorruptionRejectInitialization) {
  constexpr Corruption Changes[] = {
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  [](X64MachineState &State) { State.FP.Member ^= CorruptBit; },
#include "arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
      [](X64MachineState &State) { State.FP.Tag ^= CorruptBit; },
      [](X64MachineState &State) { State.MXCSR ^= CorruptBit; },
      [](X64MachineState &State) { State.FSBase ^= CorruptBit; },
      [](X64MachineState &State) { State.GSBase ^= CorruptBit; },
      [](X64MachineState &State) { State.UserMode = !State.UserMode; },
  };
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  for (unsigned Index = 0; Index < std::size(Changes); ++Index) {
    SCOPED_TRACE(Index);
    CorruptingTransport Machine;
    Machine.Corrupt = Changes[Index];
    EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
              x64::probe::State);
    EXPECT_EQ(Machine.Entries, 1u);
  }
}
TEST(X64MachineProbe, NopSupportDoesNotProveFloatingPointExecution) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  CorruptingTransport Machine;
  Machine.NopOnly = true;
  EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
            x64::probe::State);
  EXPECT_EQ(Machine.Entries, 2u);
}
TEST(X64MachineProbe, GenuineTransferFailureRetainsDiagnosticAndReleasesLease) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  CorruptingTransport Machine;
  Machine.FailTransfer = true;
  EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
            diagnostic::KvmState);
  EXPECT_EQ(Machine.Entries, 1u);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
}
TEST(X64MachineProbe, ActiveExecutionLeaseRejectsInitializationBeforeEntry) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  llvm::cantFail(Memory->beginRun());
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  CorruptingTransport Machine;
  EXPECT_EQ(llvm::toString(verifyX64Machine(Machine, *Memory)),
            diagnostic::Running);
  EXPECT_EQ(Machine.Entries, 0u);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), diagnostic::Running);
}
} // namespace
} // namespace neverd::emulation
