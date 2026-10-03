//===- CPUArchitectureTests.cpp - Multi-ISA execution contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <cstdlib>

namespace neverd::emulation {
namespace {
#define NEVERD_CPU_TEST_CODE(Name, ...)                                        \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#define NEVERD_CPU_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CPU_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "CPUArchitectureCases.def"
#undef NEVERD_CPU_TEST_CODE
#undef NEVERD_CPU_TEST_VALUE
#undef NEVERD_CPU_TEST_TEXT
llvm::Error code(ExecutionBackend &CPU, llvm::ArrayRef<uint32_t> Words) {
  std::vector<uint8_t> Bytes(Words.size() * sizeof(uint32_t));
  for (size_t N = 0; N < Words.size(); ++N)
    llvm::support::endian::write32le(Bytes.data() + N * sizeof(uint32_t),
                                     Words[N]);
  return CPU.write(Code, Bytes);
}
llvm::Error steps(ExecutionBackend &CPU, unsigned Count) {
  unsigned Seen = 0;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    if (Seen++ == Count)
      CPU.stop();
  };
  if (auto E = CPU.installHooks(std::move(Hooks)))
    return E;
  auto Result = CPU.run(Code, Timeout);
  llvm::consumeError(CPU.installHooks({}));
  return Result;
}
#if defined(_WIN32)
constexpr auto Native = ExecutionBackendKind::WHP;
#elif defined(__APPLE__)
constexpr auto Native = ExecutionBackendKind::HVF;
#else
constexpr auto Native = ExecutionBackendKind::KVM;
#endif
class AArch64CPU : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B =
        createExecutionBackend(GetParam(), ExecutionContract::CheckedAArch64,
                               Limit, GuestArchitecture::AArch64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !std::getenv(RequireNative) &&
          !requireHvf(GetParam(), GuestArchitecture::AArch64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    ASSERT_EQ(llvm::toString(CPU->map(Code, PageSize, Read | Write | Execute)),
              "");
    ASSERT_EQ(llvm::toString(CPU->map(Data, PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->map(Stack, PageSize, Read | Write)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X1, Data)), "");
    ASSERT_EQ(
        llvm::toString(CPU->setReg(AArch64Register::SP, Stack + PageSize)), "");
  }
};
TEST_P(AArch64CPU, ExecutesIntegerBranchAndMemoryAtHighAddresses) {
  ASSERT_EQ(llvm::toString(code(*CPU, Arithmetic)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 7)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::X0), ArithmeticValue);
  EXPECT_EQ(*CPU->reg(AArch64Register::X2), ArithmeticValue);
  EXPECT_EQ(*CPU->reg(AArch64Register::X3), 0u);
  EXPECT_EQ(*CPU->reg(AArch64Register::X4), 7u);
  EXPECT_EQ(*CPU->reg(AArch64Register::NZCV), ArithmeticFlags);
  EXPECT_EQ(*CPU->readInteger(Data, sizeof(uint64_t)), ArithmeticValue);
}
TEST_P(AArch64CPU, PreservesLinkRegisterAndReturnControlFlow) {
  ASSERT_EQ(llvm::toString(code(*CPU, Call)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 5)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::X0), 12u);
  EXPECT_EQ(*CPU->reg(AArch64Register::X30), Code + 2 * sizeof(uint32_t));
  EXPECT_EQ(*CPU->reg(AArch64Register::PC), Code + 5 * sizeof(uint32_t));
}
TEST_P(AArch64CPU, PairWritebackAndZeroExtendedRegisters) {
  ASSERT_EQ(llvm::toString(code(*CPU, PairStack)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X2, Updated)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 2)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::SP), Stack + PageSize);
  EXPECT_EQ(*CPU->reg(AArch64Register::X3), Value);
  EXPECT_EQ(*CPU->reg(AArch64Register::X4), Updated);
}
TEST_P(AArch64CPU, IndexedMemoryUsesTheArchitecturalAddress) {
  ASSERT_EQ(llvm::toString(code(*CPU, Indexed)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X2, 3)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 2)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::X3), uint32_t(Value));
  EXPECT_EQ(*CPU->readInteger(Data + 3 * sizeof(uint32_t), sizeof(uint32_t)),
            uint32_t(Value));
}
TEST_P(AArch64CPU, SignedLoadsPreserveRegisterWidthSemantics) {
  ASSERT_EQ(llvm::toString(code(*CPU, SignedLoads)), "");
  ASSERT_EQ(
      llvm::toString(CPU->writeInteger(Data, UINT64_MAX, sizeof(uint64_t))),
      "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 3)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::X0), UINT64_MAX);
  EXPECT_EQ(*CPU->reg(AArch64Register::X2), UINT32_MAX);
  EXPECT_EQ(*CPU->reg(AArch64Register::X3), UINT64_MAX);
}
TEST_P(AArch64CPU, ObserverStopsStoreBeforeEffects) {
  ASSERT_EQ(llvm::toString(code(*CPU, Store)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  BackendHooks H;
  H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
    EXPECT_EQ(A, Data);
    EXPECT_EQ(N, sizeof(uint64_t));
    EXPECT_EQ(V, Value);
    CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(H))), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::PC), Code);
  EXPECT_EQ(*CPU->readInteger(Data, sizeof(uint64_t)), 0u);
}
TEST_P(AArch64CPU, PairFaultPrecedesBothWritesAndCannotBeRestoredAway) {
  ASSERT_EQ(llvm::toString(code(*CPU, PairStore)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X1,
                                       Data + PageSize - sizeof(uint64_t))),
            "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(bool(Saved));
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::UnmappedMemory);
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  ASSERT_EQ(llvm::toString(
                CPU->snapshotBacking(Data + PageSize - Bytes.size(), Bytes)),
            "");
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data()), 0u);
  EXPECT_NE(llvm::toString(CPU->restoreContext(**Saved)), "");
}
TEST_P(AArch64CPU, ContextRestoresRegistersAgainstCurrentAliasMappings) {
  ASSERT_EQ(llvm::toString(code(*CPU, Load)), "");
  ASSERT_EQ(llvm::toString(CPU->mapAlias(Alias, Data, PageSize, Read)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, Value, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Stack, Updated, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X1, Alias)), "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(bool(Saved));
  for (unsigned N = 0; N < Remaps; ++N) {
    const uint64_t Source = N % 2 ? Stack : Data;
    ASSERT_EQ(llvm::toString(CPU->replaceAliases(
                  {{Alias, PageSize}}, {{Alias, Source, PageSize, Read}})),
              "");
    ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
    ASSERT_EQ(llvm::toString(steps(*CPU, 1)), "");
    EXPECT_EQ(*CPU->reg(AArch64Register::X0), N % 2 ? Updated : Value);
  }
}
TEST_P(AArch64CPU, ChangedCodeInvalidatesCachedInstructions) {
  ASSERT_EQ(llvm::toString(code(*CPU, Load)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 1)), "");
  ASSERT_EQ(llvm::toString(code(*CPU, Modified)), "");
  ASSERT_EQ(llvm::toString(steps(*CPU, 1)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::X0), 123u);
}
TEST_P(AArch64CPU, UnsupportedSystemInstructionStopsBeforeEffect) {
  ASSERT_EQ(llvm::toString(code(*CPU, System)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(AArch64Register::X0, Value)), "");
  EXPECT_NE(llvm::toString(CPU->run(Code, Timeout)), "");
  EXPECT_EQ(*CPU->reg(AArch64Register::X0), Value);
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::InvalidInstruction);
}
TEST_P(AArch64CPU, LoopHonorsDeadline) {
  ASSERT_EQ(llvm::toString(code(*CPU, Loop)), "");
  ASSERT_EQ(llvm::toString(CPU->run(Code, ShortTimeout)), "");
  EXPECT_TRUE(CPU->timedOut());
}
INSTANTIATE_TEST_SUITE_P(Checked, AArch64CPU,
                         testing::Values(ExecutionBackendKind::Unicorn,
                                         Native));

TEST(CPUSelection, MatchingGuestUsesTheNativeHostBackend) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) ||          \
    defined(_M_ARM64)
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto ISA = GuestArchitecture::X64;
  constexpr auto Contract = ExecutionContract::CheckedX64;
#else
  constexpr auto ISA = GuestArchitecture::AArch64;
  constexpr auto Contract = ExecutionContract::CheckedAArch64;
#endif
  auto B =
      createExecutionBackend(ExecutionBackendKind::Auto, Contract, Limit, ISA);
  if (!B) {
    auto E = B.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !std::getenv(RequireNative) && !requireHvf(Native, ISA))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  EXPECT_EQ(B->Kind, Native);
#else
  EXPECT_EQ(B->Kind, ExecutionBackendKind::Unicorn);
#endif
#else
  GTEST_SKIP();
#endif
}

TEST(UnicornAArch64, ExecutesSIMDAndTLSAndRestoresCPUOnly) {
  auto B = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                  ExecutionContract::Software, Limit,
                                  GuestArchitecture::AArch64);
  ASSERT_TRUE(bool(B)) << llvm::toString(B.takeError());
  auto &CPU = *B->CPU;
  ASSERT_EQ(llvm::toString(CPU.map(Code, PageSize, Read | Write | Execute)),
            "");
  ASSERT_EQ(llvm::toString(CPU.map(Data, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(code(CPU, CurrentEL)), "");
  ASSERT_EQ(llvm::toString(steps(CPU, 1)), "");
  EXPECT_EQ(*CPU.reg(AArch64Register::X0), 4u);
  ASSERT_EQ(llvm::toString(code(CPU, SIMD)), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::X3, Value)), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::TPIDR_EL0, Data)), "");
  ASSERT_EQ(llvm::toString(steps(CPU, 4)), "");
  ASSERT_FALSE(CPU.fault()) << std::hex << (CPU.fault() ? CPU.fault()->PC : 0);
  EXPECT_EQ(*CPU.vector(0), (RegisterValue{SIMDPattern, SIMDPattern}));
  EXPECT_EQ(*CPU.vector(1), (RegisterValue{}));
  EXPECT_EQ(*CPU.vector(2), (RegisterValue{Value, 0}));
  EXPECT_EQ(*CPU.reg(AArch64Register::X4), Data);
  auto Saved = CPU.saveContext();
  ASSERT_TRUE(bool(Saved));
  ASSERT_EQ(llvm::toString(CPU.setVector(0, {})), "");
  ASSERT_EQ(llvm::toString(CPU.setReg(AArch64Register::TPIDR_EL0, Stack)), "");
  ASSERT_EQ(llvm::toString(CPU.writeInteger(Data, Updated, sizeof(uint64_t))),
            "");
  ASSERT_EQ(llvm::toString(CPU.restoreContext(**Saved)), "");
  EXPECT_EQ(*CPU.vector(0), (RegisterValue{SIMDPattern, SIMDPattern}));
  EXPECT_EQ(*CPU.reg(AArch64Register::TPIDR_EL0), Data);
  EXPECT_EQ(*CPU.readInteger(Data, sizeof(uint64_t)), Updated);
  auto Invalid = CPU.reg(X64Register::AX);
  EXPECT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  EXPECT_NE(llvm::toString(CPU.setXmm(0, {})), "");
}
TEST(CPUSelection, GuestAndContractAreIndependentOfTheHostOS) {
  for (auto ISA : {GuestArchitecture::X64, GuestArchitecture::AArch64}) {
    auto B = createExecutionBackend(ExecutionBackendKind::Auto,
                                    ExecutionContract::Software, Limit, ISA);
    ASSERT_TRUE(bool(B)) << llvm::toString(B.takeError());
    EXPECT_EQ(B->Kind, ExecutionBackendKind::Unicorn);
    EXPECT_EQ(B->CPU->architecture(), ISA);
  }
  auto Mismatch = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                         ExecutionContract::CheckedX64, Limit,
                                         GuestArchitecture::AArch64);
  ASSERT_FALSE(bool(Mismatch));
  llvm::consumeError(Mismatch.takeError());
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto Guest = GuestArchitecture::AArch64;
  constexpr auto Contract = ExecutionContract::CheckedAArch64;
#elif defined(__aarch64__) || defined(_M_ARM64)
  constexpr auto Guest = GuestArchitecture::X64;
  constexpr auto Contract = ExecutionContract::CheckedX64;
#else
  return;
#endif
  auto Cross = createExecutionBackend(ExecutionBackendKind::Auto, Contract,
                                      Limit, Guest);
  ASSERT_TRUE(bool(Cross)) << llvm::toString(Cross.takeError());
  EXPECT_EQ(Cross->Kind, ExecutionBackendKind::Unicorn);
  EXPECT_EQ(Cross->Reason, execution::CrossISASelection);
  auto Hardware = createExecutionBackend(Native, Contract, Limit, Guest);
  ASSERT_FALSE(bool(Hardware));
  auto E = Hardware.takeError();
  EXPECT_TRUE(E.isA<BackendUnavailableError>());
  llvm::consumeError(std::move(E));
}
} // namespace
} // namespace neverd::emulation
