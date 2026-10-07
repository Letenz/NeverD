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

TEST_P(AndroidSyscall, ExplicitKernelAbsenceRetainsRawAndBionicErrorEncoding) {
  Options.LinuxKernel.emplace().UnavailableSyscalls.insert(
      LinuxUnavailableSyscall::PidFDOpen);
  auto R = run("syscall_unavailable_kernel", {Buffer});
  returned(R);
  words(R, {uint64_t(0) - 38, 77, UINT64_MAX, 38, UINT64_MAX, 38});
  ASSERT_EQ(R.Services.size(), 1u);
  EXPECT_EQ(R.Services.front().Number, 434u);
  EXPECT_EQ(R.Services.front().Result, uint64_t(0) - 38);
  Options.LinuxKernel.reset();
  R = run("syscall_unavailable_kernel", {Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(R.Services.back().Result);
}

TEST_P(AndroidSyscall,
       ReleasedGKIProcessDescriptorsShareRawAndBionicOwnership) {
  struct KernelCase {
    AndroidGKIKernel Kernel;
    const char *Label;
    bool ThreadFlag;
  };
  constexpr KernelCase Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error)  \
  {AndroidGKIKernel::Name, Label, ThreadFlag},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    Options.LinuxFiles.emplace().DescriptorLimit = 5;
    auto R = run("syscall_gki_pidfd", {Buffer, K.ThreadFlag});
    returned(R);
    words(R, {3, 0, 3, 0, UINT64_MAX, 22, uint64_t(0) - 22, 91});
    Options.LinuxFiles->DescriptorLimit = 3;
    R = run("syscall_gki_pidfd_limit", {Buffer});
    returned(R);
    words(R, {uint64_t(0) - 24, 77, UINT64_MAX, 24, 0, 0, 0, 24});
  }
}

TEST_P(AndroidSyscall, ReleasedGKIVectorImportRetainsRawAndBionicErrors) {
  struct KernelCase {
    AndroidGKIKernel Kernel;
    const char *Label;
    bool SingleBuffer;
  };
  constexpr KernelCase Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error)  \
  {AndroidGKIKernel::Name, Label, SingleBuffer},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    Options.LinuxKernel.emplace().GKI = K.Kernel;
    Options.LinuxFiles.emplace().DescriptorLimit = 5;
    auto R = run("syscall_gki_vectors", {Buffer});
    returned(R);
    const uint64_t Error = K.SingleBuffer ? 22 : 14;
    words(R, {uint64_t(0) - Error, 77, UINT64_MAX, Error, uint64_t(0) - Error,
              UINT64_MAX, Error, Error});
    EXPECT_TRUE(R.StandardOutput.empty());
    EXPECT_TRUE(R.StandardError.empty());
  }
}
TEST_P(AndroidSyscall, ReleasedGKICatalogueRetainsRawAndBionicLookupErrors) {
  struct KernelCase {
    AndroidGKIKernel Kernel;
    const char *Label;
    bool ThreadFlag;
    uint32_t NonLeader;
  };
  constexpr KernelCase Kernels[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error)  \
  {AndroidGKIKernel::Name, Label, ThreadFlag, Error},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (const auto &K : Kernels) {
    SCOPED_TRACE(K.Label);
    auto &Kernel = Options.LinuxKernel.emplace();
    Kernel.GKI = K.Kernel;
    Kernel.Tasks.emplace().emplace(2000, LinuxKernelTask{true});
    Kernel.Tasks->emplace(3000, LinuxKernelTask{false});
    Options.LinuxFiles.emplace().DescriptorLimit = 5;
    auto R = run("syscall_gki_tasks", {Buffer, K.ThreadFlag, K.NonLeader});
    returned(R);
    words(R, {uint64_t(0) - 3, 77, UINT64_MAX, 3, uint64_t(0) - K.NonLeader,
              UINT64_MAX, K.NonLeader, K.ThreadFlag ? 3u : UINT64_MAX});
    Options.LinuxFiles->DescriptorLimit = 3;
    R = run("syscall_gki_tasks_full", {Buffer});
    returned(R);
    words(R, {uint64_t(0) - 3, 77, UINT64_MAX, 3, uint64_t(0) - K.NonLeader,
              UINT64_MAX, K.NonLeader, uint64_t(0) - 24});
  }
}

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

TEST_P(AndroidSyscall, ReleasedGKICurrentTaskClockNamesItsProcessObservation) {
  Options.LinuxKernel.emplace().GKI = AndroidGKIKernel::Android17_6_18;
  Options.LinuxTime.emplace().Clocks[2] = {3, 4};
  Options.Android->ThreadLimit = 2;
  auto R = run("syscall_current_task_cpu_clock", {Buffer});
  returned(R);
  words(R, {1001, 0, 3, 4, 0, 3, 4, 87});
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Finished);
  EXPECT_TRUE(R.NativeThreads[1].Retired);
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
