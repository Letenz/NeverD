//===- AndroidResidencyTests.cpp - Shared mincore error semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x22000000;
class AndroidResidency : public testing::TestWithParam<
                             std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->EntrySymbol = "residency_call";
    Options.Android->Memory.push_back(
        {Buffer, 4096, std::vector<uint8_t>(64, 0xa5), false});
    Options.Android->ReadMemory = {{Buffer, 64}};
#endif
  }
  ProcessResult run(unsigned Op, uint64_t Address, uint64_t Length,
                    uint64_t Vector) {
    Options.Android->Arguments = {Op, Address, Length, Vector, Buffer + 60};
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
};

TEST_P(AndroidResidency, RawVariadicAndNamedCallsKeepOrderedErrorsAndBytes) {
  struct Case {
    uint64_t Address, Length, Vector;
    unsigned Error;
  };
  const Case Cases[] = {
      {1, 0, UINT64_MAX, 22},    {UINT64_MAX - 4095, 4096, UINT64_MAX, 12},
      {0, 4097, UINT64_MAX, 14}, {0, 4096, 0, 12},
      {0, 4097, Buffer, 12},     {0, 0, 0, 0},
      {0, 0, Buffer, 0}};
  for (unsigned Op : {0, 1, 2}) {
    for (const Case &C : Cases) {
      auto R = run(Op, C.Address, C.Length, C.Vector);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, !C.Error  ? 0
                               : Op == 2 ? uint64_t(0) - C.Error
                                         : UINT64_MAX);
      std::vector<uint8_t> Expected(64, 0xa5);
      llvm::support::endian::write32le(Expected.data() + 60,
                                       C.Error && Op != 2 ? C.Error : 73);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidResidency, ABackingMappingDoesNotInventPageResidency) {
  for (unsigned Op : {0, 1, 2}) {
    auto R = run(Op, Buffer, 4096, Buffer + 8);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("page residency"), std::string::npos);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(64, 0xa5));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidResidency,
    testing::Combine(
        testing::Values("residency-O0-none", "residency-O0-android",
                        "residency-O0-relr", "residency-O2-none",
                        "residency-O2-android", "residency-O2-relr"),
        testing::Values(ExecutionBackendKind::Unicorn,
                        ExecutionBackendKind::HVF, ExecutionBackendKind::KVM,
                        ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
