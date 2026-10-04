//===- InstructionFetchTests.cpp - Fresh bytes and reusable decoding
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "HvfTestPolicy.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_FETCH_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_FETCH_X64(Name, ...)                                            \
  constexpr uint8_t Name##X64[] = {__VA_ARGS__};
#define NEVERD_FETCH_ARM(Name, ...)                                            \
  constexpr uint32_t Name##ARM[] = {__VA_ARGS__};
#include "InstructionFetchCases.def"
#undef NEVERD_FETCH_ARM
#undef NEVERD_FETCH_X64
#undef NEVERD_FETCH_VALUE

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
};
constexpr Profile Profiles[] = {
#define NEVERD_FETCH_PROFILE(Name, Backend, ISA, Contract)                     \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract},
#include "InstructionFetchCases.def"
#undef NEVERD_FETCH_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }

class InstructionFetch : public testing::TestWithParam<Profile> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  bool X64;
  CPURegister Result, Pointer, Source, PCRegister;
  void SetUp() override {
    const auto &P = GetParam();
    auto B = createExecutionBackend(P.Backend, P.Contract, Limit, P.ISA);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(P.Backend, P.ISA))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    X64 = P.ISA == GuestArchitecture::X64;
    Result = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
    Pointer = X64 ? CPURegister::X64CX : CPURegister::AArch64X1;
    Source = X64 ? CPURegister::X64DX : CPURegister::AArch64X2;
    PCRegister = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    // Keep neighboring virtual code pages in distinct physical allocations.
    llvm::cantFail(
        CPU->map(Data, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Code, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->writeRegister(Pointer, {Data, 0}));
  }
  std::vector<uint8_t> bytes(llvm::ArrayRef<uint8_t> X,
                             llvm::ArrayRef<uint32_t> A) const {
    if (X64)
      return {X.begin(), X.end()};
    std::vector<uint8_t> B(A.size() * sizeof(uint32_t));
    for (size_t I = 0; I < A.size(); ++I)
      llvm::support::endian::write32le(B.data() + I * sizeof(uint32_t), A[I]);
    return B;
  }
  uint64_t value(CPURegister R) {
    return llvm::cantFail(CPU->readRegister(R))[0];
  }
  uint64_t nopSize() const { return X64 ? sizeof(NopX64) : sizeof(NopARM); }
  ExecutionExit run(uint64_t Begin, uint64_t End, BackendHooks Hooks = {}) {
    auto Observer = std::move(Hooks.Instruction);
    Hooks.Instruction = [&, End, Observer = std::move(Observer)](uint64_t PC,
                                                                 uint32_t N) {
      if (Observer)
        Observer(PC, N);
      if (PC == End)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Begin, Timeout));
  }
  ExecutionExit program(llvm::ArrayRef<uint8_t> B, BackendHooks Hooks = {}) {
    llvm::cantFail(CPU->write(Code, B));
    return run(Code, Code + B.size() - nopSize(), std::move(Hooks));
  }
};

TEST_P(InstructionFetch, AlternatingFormsKeepFreshOperandsAndRelativeBranches) {
  unsigned Seen = 0, Reads = 0, Writes = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) { ++Seen; };
  Hooks.Read = [&](uint64_t A, uint32_t N) {
    EXPECT_EQ(A, Data);
    EXPECT_EQ(N, sizeof(uint8_t));
    ++Reads;
  };
  Hooks.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
    EXPECT_EQ(A, Data);
    EXPECT_EQ(N, sizeof(uint32_t));
    EXPECT_EQ(V, MixedMemory);
    ++Writes;
  };
  const auto Exit = program(bytes(MixedX64, MixedARM), std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Seen, MixedObservations);
  EXPECT_EQ(Reads, 1u);
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(value(Result), MixedResult);
  EXPECT_EQ(value(Source), MixedByte);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, sizeof(uint32_t))),
            MixedMemory);
}

TEST_P(InstructionFetch, GuestAliasStoreChangesTheNextInstruction) {
  llvm::cantFail(CPU->writeRegister(
      Pointer, {Alias + (X64 ? X64PatchOffset : ARMPatchOffset), 0}));
  llvm::cantFail(
      CPU->writeRegister(Source, {X64 ? SecondValue : ARMReplacement, 0}));
  const auto Exit = program(bytes(RewriteX64, RewriteARM));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(value(Result), SecondValue);
}

TEST_P(InstructionFetch, HostAliasWriteAndContextRestoreUseFreshBytes) {
  auto Saved = llvm::cantFail(CPU->saveContext());
  auto Exit = program(bytes(FirstX64, FirstARM));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(value(Result), FirstValue);
  const auto Next = bytes(SecondX64, SecondARM);
  llvm::cantFail(CPU->write(Alias, Next));
  llvm::cantFail(CPU->restoreContext(*Saved));
  Exit = run(Code, Code + Next.size() - nopSize());
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(value(Result), SecondValue);
  EXPECT_EQ(value(PCRegister), Code + Next.size() - nopSize());
}

TEST_P(InstructionFetch, ExecutePermissionIsRecheckedAfterResumption) {
  auto Exit = program(bytes(FirstX64, FirstARM));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  llvm::cantFail(CPU->protect(Code, PageSize, Read | Write | UserAccessible));
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) { ++Seen; };
  Exit = run(Code, Code, std::move(Hooks));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Code);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Execute);
  EXPECT_EQ(Seen, 0u);
  EXPECT_EQ(value(Result), FirstValue);
}

TEST_P(InstructionFetch, PageTailInstructionDoesNotRequireLookaheadBytes) {
  const uint64_t Start = Code + PageSize - nopSize();
  llvm::cantFail(CPU->write(Start, bytes(NopX64, NopARM)));
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t N) {
    EXPECT_EQ(PC, Start);
    EXPECT_EQ(N, nopSize());
    ++Seen;
  };
  const auto Exit = run(Start, Code + PageSize, std::move(Hooks));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Address, Code + PageSize);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Execute);
  EXPECT_EQ(value(PCRegister), Code + PageSize);
  EXPECT_EQ(Seen, 1u);
}

TEST_P(InstructionFetch, CrossPageFetchUsesSeparateBacking) {
  llvm::cantFail(CPU->map(Code + PageSize, PageSize,
                          Read | Write | Execute | UserAccessible));
  const auto Program = bytes(FirstX64, FirstARM);
  const uint64_t Start = Code + PageSize - CrossingPrefix;
  llvm::cantFail(CPU->write(Start, Program));
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t N) {
    EXPECT_EQ(PC, Seen ? Start + Program.size() - nopSize() : Start);
    EXPECT_EQ(N, Seen ? nopSize() : Program.size() - nopSize());
    ++Seen;
  };
  const auto Exit =
      run(Start, Start + Program.size() - nopSize(), std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(value(Result), FirstValue);
  EXPECT_EQ(Seen, 2u);
}

TEST_P(InstructionFetch, InvalidTailNeverExecutesThePreviousInstruction) {
  auto Exit = program(bytes(FirstX64, FirstARM));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  const auto Invalid = bytes(InvalidTailX64, InvalidTailARM);
  const uint64_t Start = Code + PageSize - Invalid.size();
  llvm::cantFail(CPU->write(Start, Invalid));
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) { ++Seen; };
  Exit = run(Start, Start, std::move(Hooks));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
      << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::InvalidInstruction);
  EXPECT_EQ(value(PCRegister), Start);
  EXPECT_EQ(value(Result), FirstValue);
  EXPECT_EQ(Seen, 0u);
}

TEST_P(InstructionFetch, ReentrantRunCannotOverwriteTheOuterInstruction) {
  const auto Nested = bytes(SecondX64, SecondARM);
  llvm::cantFail(CPU->write(Data, Nested));
  unsigned Attempts = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC != Code)
      return;
    auto Result = CPU->runUntilExit(Data, Timeout);
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    ++Attempts;
  };
  const auto Exit = program(bytes(FirstX64, FirstARM), std::move(Hooks));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  EXPECT_EQ(Attempts, 1u);
  EXPECT_EQ(value(Result), FirstValue);
}

INSTANTIATE_TEST_SUITE_P(Checked, InstructionFetch, testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &P) {
                           return P.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
