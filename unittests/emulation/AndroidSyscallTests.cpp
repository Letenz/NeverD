//===- AndroidSyscallTests.cpp - Bionic syscall ABI workloads -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidSyscall : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 64});
    Options.InstructionQuantum = 3;
#endif
  }
  ProcessResult run(llvm::StringRef Entry,
                    std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry.str();
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.ExitStatus);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    EXPECT_FALSE(R.TraceTruncated);
  }
  void words(const ProcessResult &R, llvm::ArrayRef<uint64_t> Expected) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    const auto &Bytes = R.MemorySnapshots.front().Bytes;
    ASSERT_EQ(Bytes.size(), 64u);
    for (size_t I = 0; I < Expected.size(); ++I)
      EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8 * I),
                Expected[I]);
    for (size_t I = Expected.size() * 8; I < Bytes.size(); ++I)
      EXPECT_EQ(Bytes[I], 0);
  }
};

TEST_P(AndroidSyscall, NamedRawAndVariadicIdentityQueriesAgree) {
  auto R = run("syscall_identities", {Buffer});
  returned(R);
  words(R, {1000, 1000, 1000, 1000, 1000, 1000});
  const std::array<uint64_t, 6> Numbers = {172, 178, 174, 175, 176, 177};
  ASSERT_EQ(R.Services.size(), Numbers.size());
  size_t I = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name != "syscall")
      continue;
    ASSERT_LT(I, Numbers.size());
    EXPECT_EQ(Call.Arguments[0], Numbers[I]);
    EXPECT_EQ(Call.Result, 1000u);
    EXPECT_EQ(R.Services[I].Number, Numbers[I]);
    EXPECT_EQ(R.Services[I].Result, Call.Result);
    ++I;
  }
  EXPECT_EQ(I, Numbers.size());
}

TEST_P(AndroidSyscall, ErrorsConvertToMinusOneAndSuccessPreservesErrno) {
  auto R = run("syscall_errors", {Buffer});
  returned(R);
  words(R, {uint64_t(0) - 9, 77, UINT64_MAX, 9, UINT64_MAX, 14, 0, 14});
  ASSERT_EQ(R.Services.size(), 1u);
  EXPECT_EQ(R.Services.front().Result, uint64_t(0) - 9);
  EXPECT_TRUE(R.StandardOutput.empty());
  EXPECT_TRUE(R.StandardError.empty());
}

TEST_P(AndroidSyscall, SixArgumentsAndFullWidthPointersShareMemoryState) {
  auto R = run("syscall_memory", {Buffer});
  returned(R);
  words(R, {0x180000000, 'N', 77, UINT64_MAX, 14});
  EXPECT_EQ(R.StandardOutput, "M");
  EXPECT_TRUE(R.Services.empty());
  auto Call = llvm::find_if(R.NativeCalls, [](const auto &E) {
    return E.Name == "syscall" && E.Arguments[1] == 0x180000000;
  });
  ASSERT_NE(Call, R.NativeCalls.end());
  const std::array<uint64_t, 8> Expected = {222,  0x180000000, 4096, 3,
                                            0x22, UINT64_MAX,  0,    1};
  EXPECT_EQ(Call->Arguments, Expected);
  EXPECT_EQ(Call->Result, 0x180000000u);
}

TEST_P(AndroidSyscall, VectoredOutputAndBudgetUseSharedKernelSink) {
  auto R = run("syscall_output");
  returned(R);
  EXPECT_EQ(R.StandardError, std::string("A\0\xff\n", 4));
  EXPECT_TRUE(R.StandardOutput.empty());
  EXPECT_TRUE(R.Services.empty());
  Options.OutputLimit = 3;
  Options.Android->TraceLimit = 0;
  Options.Android->ReadMemory.clear();
  R = run("syscall_output");
  EXPECT_EQ(R.Stop, ProcessStopReason::OutputLimit) << R.Diagnostic;
  EXPECT_TRUE(R.StandardError.empty());
  EXPECT_TRUE(R.StandardOutput.empty());
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "syscall");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidSyscall, UnsupportedNumbersAndMemoryPolicyNeverReturnSuccess) {
  for (uint64_t Number :
       std::array<uint64_t, 4>{198, 9999, 0x1000000b2, UINT64_MAX}) {
    SCOPED_TRACE(Number);
    auto R = run("syscall_invoke", {Number});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "syscall");
    EXPECT_EQ(R.NativeCalls.back().Arguments[0], Number);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_TRUE(R.Services.empty());
  }
  // File-backed mmap is outside the bounded anonymous-memory profile.
  auto R = run("syscall_invoke", {222, 0, 4096, 3, 2, 0, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_FALSE(R.ReturnValue);
}

TEST_P(AndroidSyscall, ExitServicesDoNotResumeTheNativeCaller) {
  for (uint64_t Number : {93u, 94u}) {
    auto R = run("syscall_invoke", {Number, 0x1234});
    EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0x34u);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_EQ(R.NativeCalls.size(), 1u);
    EXPECT_EQ(R.NativeCalls.front().Name, "syscall");
    EXPECT_FALSE(R.NativeCalls.front().Result);
    EXPECT_TRUE(R.Services.empty());
  }
}

TEST_P(AndroidSyscall, DynamicProviderIdentityAndLifetimeRemainAuthoritative) {
  Options.Android->Libraries["libservice.so"] = {"syscall"};
  returned(run("syscall_dynamic", {0}));
  Options.Android->DefaultScope = std::vector<std::string>{"libservice.so"};
  auto R = run("syscall_dynamic", {1});
  returned(R);
  const auto Lookup = llvm::find_if(R.NativeCalls, [](const auto &E) {
    return E.Name == "dlsym" && E.Symbol == "syscall";
  });
  const auto Call = llvm::find_if(
      R.NativeCalls, [](const auto &E) { return E.Name == "syscall"; });
  ASSERT_NE(Lookup, R.NativeCalls.end());
  ASSERT_NE(Call, R.NativeCalls.end());
  EXPECT_EQ(Call->Library, "libservice.so");
  EXPECT_EQ(Call->PC, Lookup->Result);
  EXPECT_EQ(Call->Arguments[0], 178u);
  EXPECT_EQ(Call->Result, 1000u);
  EXPECT_TRUE(R.Services.empty());
  Options.Android->DefaultScope.reset();
  R = run("syscall_dynamic", {1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "syscall");
  EXPECT_EQ(R.NativeCalls.back().Library, "libservice.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

INSTANTIATE_TEST_SUITE_P(Compiled, AndroidSyscall,
                         testing::Values("syscall-O0-none",
                                         "syscall-O0-android",
                                         "syscall-O0-relr", "syscall-O2-none",
                                         "syscall-O2-android",
                                         "syscall-O2-relr"));
} // namespace
} // namespace neverd::emulation
