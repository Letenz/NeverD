//===- HvfTests.cpp - Native host execution and process VM isolation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <cstdlib>
#include <thread>

namespace neverd::emulation {
namespace {
#if defined(__aarch64__) || defined(__arm64__)
constexpr auto ISA = GuestArchitecture::AArch64;
constexpr auto Contract = ExecutionContract::CheckedAArch64;
constexpr auto Result = CPURegister::AArch64X0;
#else
constexpr auto ISA = GuestArchitecture::X64;
constexpr auto Contract = ExecutionContract::CheckedX64;
constexpr auto Result = CPURegister::X64RAX;
#endif
constexpr uint64_t Code = 0x10000, Page = 4096, Limit = 16 * Page;
class Hvf : public testing::Test {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto Backend =
        createExecutionBackend(ExecutionBackendKind::HVF, Contract, Limit, ISA);
    if (!Backend) {
      auto E = Backend.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable && !std::getenv("NEVERD_REQUIRE_HVF"))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    CPU = std::move(Backend->CPU);
  }
  void prepare(ExecutionBackend &Target, uint16_t Value) {
    ASSERT_EQ(llvm::toString(Target.map(Code, Page, Read | Write | Execute)),
              "");
#if defined(__aarch64__) || defined(__arm64__)
    uint8_t Program[8];
    llvm::support::endian::write32le(
        Program, 0xd2800000 | (uint32_t(Value) << 5));         // mov x0,#imm
    llvm::support::endian::write32le(Program + 4, 0xd503201f); // nop
#else
    uint8_t Program[] = {0xb8, uint8_t(Value), uint8_t(Value >> 8), 0, 0, 0x90};
#endif
    ASSERT_EQ(llvm::toString(Target.write(Code, Program)), "");
  }
  void run(ExecutionBackend &Target, uint64_t Value) {
    unsigned Count = 0;
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t, uint32_t) {
      if (Count++ == 1)
        Target.stop();
    };
    ASSERT_EQ(llvm::toString(Target.installHooks(std::move(Hooks))), "");
    auto E = Target.run(Code, 1000000);
    EXPECT_EQ(llvm::toString(std::move(E)), "");
    EXPECT_EQ(llvm::cantFail(Target.readRegister(Result))[0], Value);
    ASSERT_EQ(llvm::toString(Target.installHooks({})), "");
  }
};
TEST_F(Hvf, ExecutesAnInstructionAndCanBeCalledFromAnotherThread) {
  prepare(*CPU, 42);
  run(*CPU, 42);
  std::thread Other([&] { run(*CPU, 42); });
  Other.join();
}
TEST_F(Hvf, DistinctCPUsKeepTheirBackingAndSurvivePeerDestruction) {
  prepare(*CPU, 11);
  auto Peer =
      createExecutionBackend(ExecutionBackendKind::HVF, Contract, Limit, ISA);
  ASSERT_TRUE(bool(Peer)) << llvm::toString(Peer.takeError());
  prepare(*Peer->CPU, 29);
  for (unsigned N = 0; N != 8; ++N) {
    run(*CPU, 11);
    run(*Peer->CPU, 29);
  }
  Peer->CPU.reset();
  run(*CPU, 11);
}
TEST_F(Hvf, ConcurrentCallersKeepIndependentGuestRegisters) {
  prepare(*CPU, 113);
  auto Peer =
      createExecutionBackend(ExecutionBackendKind::HVF, Contract, Limit, ISA);
  ASSERT_TRUE(bool(Peer)) << llvm::toString(Peer.takeError());
  prepare(*Peer->CPU, 237);
  std::thread A([&] {
    for (unsigned N = 0; N < 20; ++N)
      run(*CPU, 113);
  });
  std::thread B([&] {
    for (unsigned N = 0; N < 20; ++N)
      run(*Peer->CPU, 237);
  });
  A.join();
  B.join();
}
TEST(HvfConfiguration, NativeSelectionAndBuildAvailabilityAreExplicit) {
  ExecutionConfiguration Config;
  Config.Architecture = ISA;
  Config.Contract = Contract;
  const auto Resolved = llvm::cantFail(resolveExecutionConfiguration(Config));
#if defined(__APPLE__)
  EXPECT_EQ(Resolved.Configuration.Backend, ExecutionBackendKind::HVF);
#endif
  Config.Architecture = ISA == GuestArchitecture::X64
                            ? GuestArchitecture::AArch64
                            : GuestArchitecture::X64;
  Config.Contract = ISA == GuestArchitecture::X64
                        ? ExecutionContract::CheckedAArch64
                        : ExecutionContract::CheckedX64;
  EXPECT_EQ(llvm::cantFail(resolveExecutionConfiguration(Config))
                .Configuration.Backend,
            ExecutionBackendKind::Unicorn);
  const auto Build = llvm::cantFail(
      queryExecutionBackendBuild(ExecutionBackendKind::HVF, ISA));
#if !defined(__APPLE__)
  EXPECT_EQ(Build.Availability, BackendAvailability::HostPlatformMismatch);
#elif !defined(NEVERD_EMULATION_HVF)
  EXPECT_EQ(Build.Availability, BackendAvailability::BuildDisabled);
#else
  EXPECT_EQ(Build.Availability, BackendAvailability::Available);
#endif
}
TEST_F(Hvf, LiveInitializationProbeDoesNotChangeAnotherCPU) {
  prepare(*CPU, 91);
  run(*CPU, 91);
  auto Probe =
      createExecutionBackend(ExecutionBackendKind::HVF, Contract, Limit, ISA);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  run(*CPU, 91);
}
TEST_F(Hvf, OneGuestPageBudgetDoesNotBecomeTheHostPageBudget) {
  auto Small =
      createExecutionBackend(ExecutionBackendKind::HVF, Contract, Page, ISA);
  ASSERT_TRUE(bool(Small)) << llvm::toString(Small.takeError());
  prepare(*Small->CPU, 7);
  run(*Small->CPU, 7);
  EXPECT_NE(llvm::toString(Small->CPU->map(Code + Page, Page, Read | Write)),
            "");
}
TEST(HvfConfiguration,
     VocabularyAndHostISARejectMismatchesBeforeInitialization) {
  EXPECT_EQ(llvm::cantFail(parseExecutionBackend("hvf")),
            ExecutionBackendKind::HVF);
  EXPECT_STREQ(executionBackendName(ExecutionBackendKind::HVF), "hvf");
  const auto Other = ISA == GuestArchitecture::X64 ? GuestArchitecture::AArch64
                                                   : GuestArchitecture::X64;
  auto Build = queryExecutionBackendBuild(ExecutionBackendKind::HVF, Other);
  ASSERT_TRUE(bool(Build)) << llvm::toString(Build.takeError());
  EXPECT_NE(Build->Availability, BackendAvailability::Available);
}
} // namespace
} // namespace neverd::emulation
