//===- ThreadPointerTests.cpp - Real TLS register and address execution ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
namespace {
#define NEVERD_THREAD_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_THREAD_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_THREAD_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ThreadPointerCases.def"
#undef NEVERD_THREAD_VALUE
#undef NEVERD_THREAD_BYTES
#undef NEVERD_THREAD_TEXT
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }

class ThreadPointer : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  bool X64;
  CPURegister Pointer, Result;
  void SetUp() override {
    const auto &P = GetParam();
    auto B = createExecutionBackend(P.Backend, P.Contract, MemoryLimit, P.ISA);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(P.Backend, P.ISA))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    X64 = P.ISA == GuestArchitecture::X64;
    Pointer = X64 ? CPURegister::X64FSBase : CPURegister::AArch64TPIDR_EL0;
    Result = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeInteger(Data, FirstValue, WordSize));
    llvm::cantFail(CPU->writeInteger(Data + WordSize, SecondValue, WordSize));
    llvm::cantFail(CPU->writeRegister(Pointer, {Data, 0}));
  }
  uint64_t value(CPURegister R) {
    return llvm::cantFail(CPU->readRegister(R))[0];
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Bytes, BackendHooks Hooks = {}) {
    llvm::cantFail(CPU->write(Code, Bytes));
    const uint64_t End = Code + Bytes.size() - (X64 ? 1 : sizeof(uint32_t));
    Hooks.Instruction = [&, End](uint64_t PC, uint32_t) {
      if (PC == End)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  llvm::ArrayRef<uint8_t> readCode() {
    return X64 ? llvm::ArrayRef(ReadX64) : llvm::ArrayRef(ReadARM);
  }
};

TEST_P(ThreadPointer, NativeReadsAndContextRestoreUseTheRetainedBase) {
  auto Saved = llvm::cantFail(CPU->saveContext());
  EXPECT_EQ(run(readCode()).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(value(Result), FirstValue);
  llvm::cantFail(CPU->writeRegister(Pointer, {Data + WordSize, 0}));
  EXPECT_EQ(run(readCode()).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(value(Result), SecondValue);
  llvm::cantFail(CPU->restoreContext(*Saved));
  EXPECT_EQ(value(Pointer), Data);
  EXPECT_EQ(run(readCode()).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(value(Result), FirstValue);
}

TEST_P(ThreadPointer, ReadObserverStopsBeforeTheTLSLoad) {
  llvm::cantFail(CPU->writeRegister(Result, {SecondValue, 0}));
  unsigned Reads = 0;
  BackendHooks Hooks;
  Hooks.Read = [&](uint64_t Address, uint32_t Size) {
    EXPECT_EQ(Address, Data);
    EXPECT_EQ(Size, WordSize);
    ++Reads;
    CPU->stop();
  };
  EXPECT_EQ(run(readCode(), std::move(Hooks)).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(Reads, 1u);
  EXPECT_EQ(value(Result), SecondValue);
  EXPECT_EQ(value(Pointer), Data);
}

TEST_P(ThreadPointer, AThreadPointerCannotExposeSupervisorMemory) {
  llvm::cantFail(CPU->protect(Data, PageSize, Read | Write));
  llvm::cantFail(CPU->writeRegister(Result, {SecondValue, 0}));
  const auto Exit = run(readCode());
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Data);
  EXPECT_EQ(value(Result), SecondValue);
}

TEST_P(ThreadPointer,
       ARMGuestWritesSurviveTransportAndDoNotAdmitOtherSystemRegisters) {
  if (X64)
    GTEST_SKIP() << RequiresARM;
  llvm::cantFail(CPU->writeRegister(Result, {Data + WordSize, 0}));
  EXPECT_EQ(run(WriteARM).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(value(Pointer), Data + WordSize);
  EXPECT_EQ(value(CPURegister::AArch64X1), Data + WordSize);
  EXPECT_EQ(run(readCode()).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(value(Result), SecondValue);
  EXPECT_EQ(run(PrivilegedARM).Kind, ExecutionExitKind::UnsupportedOperation);
}

TEST_P(ThreadPointer, X64AddressSizeTruncationPrecedesSegmentBaseAddition) {
  if (!X64)
    GTEST_SKIP() << RequiresX64;
  llvm::cantFail(CPU->writeRegister(Result, {ZeroExtendedOffset, 0}));
  EXPECT_EQ(run(Read32X64).Kind, ExecutionExitKind::Stopped);
  EXPECT_EQ(value(Result), FirstValue);
}

INSTANTIATE_TEST_SUITE_P(Transports, ThreadPointer,
                         testing::ValuesIn(Profiles));
} // namespace
} // namespace neverd::emulation
