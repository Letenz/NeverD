//===- ProcessReportTests.cpp - Lossless bounded process wire contract ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_FAULT_REPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "FaultReportCases.def"
#undef NEVERD_FAULT_REPORT_TEXT

TEST(ProcessReport, RetainsOptionalFaultCauseAndFullWidthProcessorCode) {
  ProcessResult Result{ProcessProfile::WindowsPE64,
                       GuestArchitecture::X64,
                       ExecutionBackendKind::KVM,
                       {}};
  Result.LastCPUExit = ExecutionExit{ExecutionExitKind::GuestTrap,
                                     BackendFault{BackendFaultKind::Interrupt},
                                     {}};
  auto Check = [&](std::optional<uint64_t> Code, const char *Expected) {
    for (bool Classified : {false, true}) {
      auto &Fault = *Result.LastCPUExit->Fault;
      Fault.ErrorCode = Code;
      Fault.Cause = Classified
                        ? std::optional(BackendFaultCause::OperandAlignment)
                        : std::nullopt;
      auto JSON = llvm::cantFail(llvm::json::parse(processResultJSON(Result)));
      const auto *Record = JSON.getAsObject()
                               ->getObject(field::CPUExit)
                               ->getObject(field::Fault);
      ASSERT_NE(Record, nullptr);
      if (Classified)
        EXPECT_EQ(Record->getString(field::Cause), AlignmentCause);
      else
        EXPECT_TRUE(Record->get(field::Cause)->getAsNull());
      if (Code)
        EXPECT_EQ(Record->getString(field::ErrorCode), Expected);
      else
        EXPECT_TRUE(Record->get(field::ErrorCode)->getAsNull());
    }
  };
#define NEVERD_FAULT_REPORT_CASE(Name, Code, ProcessHex, DriverHex)            \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Code, ProcessHex);                                                   \
  }
#include "FaultReportCases.def"
#undef NEVERD_FAULT_REPORT_CASE
}

TEST(ProcessReport, RejectsMalformedRequestsBeforeWorkloadConstruction) {
#define NEVERD_PROCESS_INVALID_JSON(Name, Text)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    auto Result = processOptionsFromJSON(Text);                                \
    EXPECT_FALSE(bool(Result));                                                \
    llvm::consumeError(Result.takeError());                                    \
  }
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_INVALID_JSON
  auto Large = processOptionsFromJSON(std::string(field::JSONLimit + 1, ' '));
  EXPECT_FALSE(bool(Large));
  EXPECT_EQ(llvm::toString(Large.takeError()), field::TooLarge);
}

TEST(ProcessReport, PreservesBinaryOutputRawRegisterBitsAndNullableStatus) {
  ProcessResult Result{ProcessProfile::LinuxELF64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  Result.Entry = UINT64_MAX;
  Result.StandardOutput = llvm::fromHex(BinaryHex);
  Result.Stop = ProcessStopReason::CPUFailure;
  ProcessServiceEvent Event{UINT64_MAX, UINT64_MAX, {}, UINT64_MAX};
  Event.Arguments.fill(UINT64_MAX);
  Result.Services.push_back(Event);
  Result.LastCPUExit = ExecutionExit{
      ExecutionExitKind::GuestFault,
      BackendFault{BackendFaultKind::Protection, UINT64_MAX, UINT64_MAX, 8,
                   BackendAccessKind::Write, std::nullopt},
      {}};
  auto Parsed = llvm::json::parse(processResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Object = Parsed->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->get(field::ExitStatus)->getAsNull());
  EXPECT_EQ(Object->getString(field::Entry), AllBits);
  EXPECT_EQ(Object->getString(field::Stdout), BinaryHex);
  const auto *Service =
      Object->getArray(field::Services)->front().getAsObject();
  EXPECT_EQ(Service->getString(field::PC), AllBits);
  EXPECT_EQ(Service->getString(field::Number), AllBits);
  EXPECT_EQ(Service->getString(field::Result), AllBits);
  for (const auto &Argument : *Service->getArray(field::Arguments))
    EXPECT_EQ(Argument.getAsString(), AllBits);
  const auto *Exit = Object->getObject(field::CPUExit);
  EXPECT_EQ(Exit->getString(field::Kind), FaultExit);
  EXPECT_EQ(Exit->getObject(field::Fault)->getString(field::Address), AllBits);
  Result.ExitStatus = 0;
  Parsed = llvm::json::parse(processResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->getAsObject()->getInteger(field::ExitStatus), 0);
}
TEST(ProcessReport, PreservesExplicitWindowsCatalogueAndRejectsOtherProfiles) {
  auto O = processOptionsFromJSON(WindowsModuleOptions);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Windows);
  ASSERT_EQ(O->Windows->Modules.size(), 1u);
  EXPECT_EQ(O->Windows->Modules.front().Name, WindowsModuleName);
  EXPECT_EQ(O->Windows->Modules.front().Path.generic_string(),
            WindowsModulePath);
  for (auto Profile :
       {ProcessProfile::LinuxELF64, ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess(WindowsModulePath, Profile, *O);
    EXPECT_FALSE(bool(R));
    if (!R)
      EXPECT_EQ(llvm::toString(R.takeError()), field::WindowsProfile);
  }
}
TEST(ProcessReport, LinuxClockInputIsLosslessAndRestrictedToLinuxProfiles) {
  auto O = processOptionsFromJSON(R"({"linux_time":{"clocks":[
    {"id":0,"seconds":"-9223372036854775808","nanoseconds":999999999},
    {"id":1,"seconds":"9223372036854775807","nanoseconds":0}],
    "timezone":{"minutes_west":-2147483648,"dst_time":2147483647}}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxTime);
  EXPECT_EQ(O->LinuxTime->Clocks.at(0).Seconds, INT64_MIN);
  EXPECT_EQ(O->LinuxTime->Clocks.at(1).Seconds, INT64_MAX);
  EXPECT_EQ(O->LinuxTime->Clocks.at(0).Nanoseconds, 999999999);
  EXPECT_EQ(O->LinuxTime->Timezone->MinutesWest, INT32_MIN);
  for (auto P :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::LinuxTimeProfile);
  }
  for (auto Bad : {LinuxTimespec{1, -1}, LinuxTimespec{1, 1000000000}}) {
    O->LinuxTime->Clocks[0] = Bad;
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("nanoseconds"),
              std::string::npos);
  }
  O->LinuxTime->Clocks = {{10, {1, 0}}};
  auto R =
      emulateProcess("missing.elf", ProcessProfile::AndroidNativeAArch64, *O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("clock ID"), std::string::npos);
}
TEST(ProcessReport, MalformedClockInputsFailBeforeExecution) {
  for (
      const char *Bad :
      {"null",
       "[]",
       "true",
       R"({"unknown":0})",
       R"({"clocks":{}})",
       R"({"clocks":[{"id":0,"seconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":0,"extra":0}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":0},{"id":0,"seconds":1,"nanoseconds":1}]})",
       R"({"clocks":[{"id":10,"seconds":0,"nanoseconds":0}]})",
       R"({"clocks":[{"id":-3,"seconds":0,"nanoseconds":0}]})",
       R"({"clocks":[{"id":4294967296,"seconds":0,"nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":-1}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":1000000000}]})",
       R"({"clocks":[{"id":0,"seconds":9007199254740992,"nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":1e99,"nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":"9223372036854775808","nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":"-9223372036854775809","nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":"0x1","nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":1.5,"nanoseconds":0}]})",
       R"({"timezone":{"minutes_west":0}})",
       R"({"timezone":{"minutes_west":2147483648,"dst_time":0}})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"linux_time\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST(ProcessReport, MemoryFileBytesAreExplicitAndRestrictedToLinuxProfiles) {
  auto O = processOptionsFromJSON(R"({"linux_files":{"files":[
    {"path":"/fixture/data","bytes_hex":"00ff410a805A"},
    {"path":"/empty","bytes_hex":""}],"descriptor_limit":4}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxFiles);
  EXPECT_EQ(O->LinuxFiles->DescriptorLimit, 4u);
  EXPECT_EQ(O->LinuxFiles->Files.at("/fixture/data"),
            (std::vector<uint8_t>{0, 0xff, 0x41, 0x0a, 0x80, 0x5a}));
  EXPECT_TRUE(O->LinuxFiles->Files.at("/empty").empty());
  for (auto P :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::LinuxFilesProfile);
  }
  for (unsigned Mode = 0; Mode < 5; ++Mode) {
    SCOPED_TRACE(Mode);
    O->LinuxFiles.emplace();
    auto &F = *O->LinuxFiles;
    if (Mode == 0)
      F.DescriptorLimit = 2;
    if (Mode == 1)
      F.DescriptorLimit = 4097;
    if (Mode == 2)
      F.Files["/data"].resize(16 * 1024 * 1024);
    if (Mode == 3)
      F.Files["/relative/../bad"] = {};
    if (Mode == 4)
      for (unsigned I = 0; I < 257; ++I)
        F.Files["/file" + std::to_string(I)] = {};
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
}

TEST(ProcessReport, MalformedMemoryFileCataloguesFailBeforeExecution) {
  for (
      const char *Bad :
      {"null",
       "[]",
       "{}",
       R"({"files":null})",
       R"({"files":[],"host":true})",
       R"({"files":[],"descriptor_limit":-1})",
       R"({"files":[],"descriptor_limit":2})",
       R"({"files":[],"descriptor_limit":4097})",
       R"({"files":[],"descriptor_limit":4294967296})",
       R"({"files":[],"descriptor_limit":3.5})",
       R"({"files":[{"path":"/x"}]})",
       R"({"files":[{"path":"/x","bytes_hex":"g0"}]})",
       R"({"files":[{"path":"/x","bytes_hex":"0"}]})",
       R"({"files":[{"path":"/x","bytes_hex":"","extra":1}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x","bytes_hex":""}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x/y","bytes_hex":""}]})",
       R"({"files":[{"path":"relative","bytes_hex":""}]})",
       R"({"files":[{"path":"/a/../b","bytes_hex":""}]})",
       R"({"files":[{"path":"/a//b","bytes_hex":""}]})",
       R"({"files":[{"path":"/a/./b","bytes_hex":""}]})",
       R"({"files":[{"path":"/a/","bytes_hex":""}]})",
       R"({"files":[{"path":"/","bytes_hex":""}]})",
       R"({"files":[{"path":"/a\u0000b","bytes_hex":""}]})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"linux_files\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  auto Empty = processOptionsFromJSON(R"({"linux_files":{"files":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->LinuxFiles->Files.empty());
  EXPECT_EQ(Empty->LinuxFiles->DescriptorLimit, 256u);
}
} // namespace
} // namespace neverd::emulation
