//===- LinuxKernelAvailabilityTests.cpp - Explicit absence inputs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxKernelAvailability.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"
namespace neverd::emulation {
namespace {
using namespace linux_model;
TEST(LinuxKernelAvailability, AbsenceAppliesOnlyToDeclaredInterfaces) {
  std::optional<LinuxKernelOptions> Options;
  EXPECT_FALSE(unavailableKernelService(ServiceKind::PidFDOpen, Options));
  Options.emplace();
  EXPECT_FALSE(unavailableKernelService(ServiceKind::PidFDOpen, Options));
  Options->UnavailableSyscalls.insert(LinuxUnavailableSyscall::PidFDOpen);
  EXPECT_TRUE(unavailableKernelService(ServiceKind::PidFDOpen, Options));
  EXPECT_FALSE(unavailableKernelService(ServiceKind::GetPID, Options));
  EXPECT_FALSE(unavailableKernelService(ServiceKind::SetPriority, Options));
  EXPECT_FALSE(bool(validateKernelOptions(*Options)));
  Options->UnavailableSyscalls.insert(static_cast<LinuxUnavailableSyscall>(99));
  auto E = validateKernelOptions(*Options);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), KernelOptions);
}
TEST(LinuxKernelAvailability, JSONAndTypedInputsFailBeforeLoading) {
  auto O = processOptionsFromJSON(
      R"({"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxKernel);
  EXPECT_TRUE(O->LinuxKernel->UnavailableSyscalls.contains(
      LinuxUnavailableSyscall::PidFDOpen));
  for (auto Profile :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", Profile, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()),
              process_report::LinuxKernelProfile);
  }
  O->LinuxKernel->UnavailableSyscalls.insert(
      static_cast<LinuxUnavailableSyscall>(99));
  auto Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
  for (const char *Value :
       {"null", "[]", "{}", R"({"unavailable_syscalls":null})",
        R"({"unavailable_syscalls":true})", R"({"unavailable_syscalls":[434]})",
        R"({"unavailable_syscalls":["openat"]})",
        R"({"unavailable_syscalls":["unknown"]})",
        R"({"unavailable_syscalls":["pidfd_open","pidfd_open"]})",
        R"({"unavailable_syscalls":["pidfd_open"],"extra":0})"}) {
    auto R =
        processOptionsFromJSON(std::string("{\"linux_kernel\":") + Value + "}");
    ASSERT_FALSE(bool(R)) << Value;
    llvm::consumeError(R.takeError());
  }
  auto Empty =
      processOptionsFromJSON(R"({"linux_kernel":{"unavailable_syscalls":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->LinuxKernel->UnavailableSyscalls.empty());
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class LinuxKernelProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld process fixtures unavailable";
#else
    Options.Backend = GetParam().Backend;
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GetParam().ISA;
    C.Contract = GetParam().ISA == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(GetParam().Backend, GetParam().ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Options.LinuxKernel.emplace().UnavailableSyscalls.insert(
        LinuxUnavailableSyscall::PidFDOpen);
#endif
  }
  ProcessResult run(char Mode, const char *Opt) {
    Options.Arguments = {"kernel", std::string(1, Mode)};
#ifdef NEVERD_PROCESS_FIXTURE_DIR
    auto Path =
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
        (std::string(GetParam().ISA == GuestArchitecture::X64 ? "X64"
                                                              : "AArch64") +
         "-kernel-" + Opt + ".elf");
#else
    std::filesystem::path Path;
#endif
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::LinuxELF64, Options));
  }
};
TEST_P(LinuxKernelProcess, AbsentCallReturnsENOSYSBeforeArgumentValidation) {
  for (const char *Opt : {"O0", "O2"}) {
    auto R = run('u', Opt);
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    unsigned Calls = 0;
    for (const auto &Event : R.Services) {
      if (Event.Number == 434) {
        EXPECT_EQ(Event.Result, uint64_t(0) - 38);
        ++Calls;
      }
    }
    EXPECT_EQ(Calls, 2u);
  }
}
TEST_P(LinuxKernelProcess, UnspecifiedAndOtherInterfacesKeepTheirBoundaries) {
  for (char Mode : {'a', 'e', 'x'}) {
    if (Mode == 'a')
      Options.LinuxKernel.reset();
    else {
      Options.LinuxKernel.emplace();
      if (Mode == 'x')
        Options.LinuxKernel->UnavailableSyscalls.insert(
            LinuxUnavailableSyscall::PidFDOpen);
    }
    auto R = run(Mode == 'x' ? 'x' : 'a', "O2");
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, LinuxKernelProcess,
                         testing::ValuesIn(Profiles));
} // namespace
} // namespace neverd::emulation
