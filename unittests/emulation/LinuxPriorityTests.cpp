//===- LinuxPriorityTests.cpp - Explicit per-task priority contracts -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxPriority.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"

namespace neverd::emulation {
namespace {
using namespace linux_model;
ProcessServiceEvent priorityEvent(uint64_t Which, uint64_t Who,
                                  uint64_t Nice = 0) {
  ProcessServiceEvent Event{};
  Event.Arguments[0] = Which;
  Event.Arguments[1] = Who;
  Event.Arguments[2] = Nice;
  return Event;
}
TEST(LinuxPriority, TasksRetainIndependentStateAndRefusalsDoNotWrite) {
  std::optional<LinuxPriorityOptions> Options;
  Options.emplace().Tasks = {{1000, 0}, {1001, -1}};
  LinuxPriority State(Options);
  ProcessResult Result{};
  EXPECT_EQ(State.handle(ServiceKind::SetPriority, priorityEvent(0, 0, 10),
                         1000, Result),
            0u);
  EXPECT_EQ(
      State.handle(ServiceKind::GetPriority, priorityEvent(0, 0), 1000, Result),
      10u);
  EXPECT_EQ(
      State.handle(ServiceKind::GetPriority, priorityEvent(0, 0), 1001, Result),
      21u);
  EXPECT_EQ(State.handle(ServiceKind::SetPriority, priorityEvent(0, 0, -20),
                         1000, Result),
            uint64_t(0) - 13);
  EXPECT_EQ(State.handle(ServiceKind::GetPriority, priorityEvent(0, 1000), 1001,
                         Result),
            10u);
  EXPECT_FALSE(State.handle(ServiceKind::SetPriority, priorityEvent(0, 999, 19),
                            1000, Result));
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(State.handle(ServiceKind::GetPriority, priorityEvent(0, 1000), 1001,
                         Result),
            10u);
}
TEST(LinuxPriority, MissingModelAndUnmodeledSelectorsRemainBoundaries) {
  std::optional<LinuxPriorityOptions> Options;
  LinuxPriority Missing(Options);
  ProcessResult Result{};
  EXPECT_FALSE(Missing.handle(ServiceKind::SetPriority, priorityEvent(0, 0, 10),
                              1000, Result));
  EXPECT_EQ(Result.Diagnostic, PriorityInputsMissing);
  Options.emplace().Tasks = {{1000, 0}};
  LinuxPriority State(Options);
  for (auto Which : {1u, 2u}) {
    EXPECT_FALSE(State.handle(ServiceKind::SetPriority,
                              priorityEvent(Which, 0, 10), 1000, Result));
    EXPECT_EQ(Result.Diagnostic, PrioritySelection);
  }
  EXPECT_EQ(State.handle(ServiceKind::SetPriority, priorityEvent(-1, 0, 10),
                         1000, Result),
            uint64_t(0) - 22);
  EXPECT_EQ(
      State.handle(ServiceKind::GetPriority, priorityEvent(0, 0), 1000, Result),
      20u);
}
TEST(LinuxPriority, JSONValidatesSignedStateLimitsAndProfileBeforeExecution) {
  auto O = processOptionsFromJSON(R"({"linux_priority":{
    "tasks":[{"id":1000,"nice":-20},{"id":1001,"nice":19}],
    "cap_sys_nice":true,"rlimit_nice":40}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxPriority);
  EXPECT_EQ(O->LinuxPriority->Tasks.at(1000), -20);
  EXPECT_EQ(O->LinuxPriority->Tasks.at(1001), 19);
  EXPECT_TRUE(O->LinuxPriority->CapSysNice);
  for (auto Profile :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", Profile, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()),
              process_report::LinuxPriorityProfile);
  }
  O->LinuxPriority->Tasks[1000] = -21;
  auto Invalid = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
  ASSERT_FALSE(bool(Invalid));
  EXPECT_EQ(llvm::toString(Invalid.takeError()), PriorityOptions);
  for (const char *Bad :
       {"null", "[]", "{}", R"({"tasks":{}})", R"({"tasks":[],"unknown":0})",
        R"({"tasks":[],"cap_sys_nice":1})", R"({"tasks":[],"rlimit_nice":41})",
        R"({"tasks":[{"id":0,"nice":0}]})", R"({"tasks":[{"id":-1,"nice":0}]})",
        R"({"tasks":[{"id":2147483648,"nice":0}]})",
        R"({"tasks":[{"id":1000,"nice":20}]})",
        R"({"tasks":[{"id":1000,"nice":-21}]})",
        R"({"tasks":[{"id":1000,"nice":1.5}]})",
        R"({"tasks":[{"id":1000,"nice":0,"extra":0}]})",
        R"({"tasks":[{"id":1000}]})",
        R"({"tasks":[{"id":1000,"nice":0},{"id":1000,"nice":1}]})"}) {
    auto R =
        processOptionsFromJSON(std::string("{\"linux_priority\":") + Bad + "}");
    ASSERT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
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
class LinuxPriorityProcess : public testing::TestWithParam<Profile> {
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
    Options.LinuxPriority.emplace().Tasks = {{1000, 0}, {1001, -5}};
#endif
  }
  ProcessResult run(char Mode, const char *Opt) {
    Options.Arguments = {"priority", std::string(1, Mode)};
#ifdef NEVERD_PROCESS_FIXTURE_DIR
    auto Path =
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
        (std::string(GetParam().ISA == GuestArchitecture::X64 ? "X64"
                                                              : "AArch64") +
         "-priority-" + Opt + ".elf");
#else
    std::filesystem::path Path;
#endif
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::LinuxELF64, Options));
  }
};
TEST_P(LinuxPriorityProcess, RawStateClampingPermissionsAnd32BitArguments) {
  for (const char *Opt : {"O0", "O2"}) {
    for (char Mode : {'n', 'c', 'l'}) {
      Options.LinuxPriority->CapSysNice = Mode == 'c';
      Options.LinuxPriority->RlimitNice = Mode == 'l' ? 30 : 0;
      auto R = run(Mode, Opt);
      ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
      EXPECT_EQ(R.ExitStatus, 0u);
    }
  }
}
TEST_P(LinuxPriorityProcess,
       MissingInputsUnknownTasksAndGroupScopesStopClearly) {
  for (char Mode : {'m', 'u', 'g', 's'}) {
    if (Mode == 'm')
      Options.LinuxPriority.reset();
    else
      Options.LinuxPriority.emplace().Tasks = {{1000, 0}};
    auto R = run(Mode, "O2");
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, LinuxPriorityProcess,
                         testing::ValuesIn(Profiles));
} // namespace
} // namespace neverd::emulation
