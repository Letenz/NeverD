//===- ProcessReportTests.cpp - Lossless bounded process wire contract ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
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

TEST(ProcessReport, AndroidSnapshotsRequireInitialMappingsUnlessExplicit) {
  auto O = processOptionsFromJSON(R"({"android":{"entry_symbol":"inspect",
    "read_memory":[{"address":4096,"size":64},
      {"address":8192,"size":64,"require_mapped_at_entry":false},
      {"address":12288,"size":64,"require_mapped_at_entry":true}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Android);
  const auto &Reads = O->Android->ReadMemory;
  ASSERT_EQ(Reads.size(), 3u);
  EXPECT_TRUE(Reads[0].RequireMappedAtEntry);
  EXPECT_FALSE(Reads[1].RequireMappedAtEntry);
  EXPECT_EQ(Reads[1].Address, 8192u);
  EXPECT_EQ(Reads[1].Size, 64u);
  EXPECT_TRUE(Reads[2].RequireMappedAtEntry);
  for (const char *Bad : {"null", "0", "\"false\"", "[]", "{}"}) {
    auto R = processOptionsFromJSON(
        std::string(R"({"android":{"entry_symbol":"inspect","read_memory":[
          {"address":4096,"size":64,"require_mapped_at_entry":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  auto R = processOptionsFromJSON(R"({"android":{"entry_symbol":"inspect",
    "memory":[{"address":4096,"size":4096,"require_mapped_at_entry":false}]}})");
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}

TEST(ProcessReport, ExplicitSignalActionsAreLosslessAndLinuxOnly) {
  auto O = processOptionsFromJSON(R"({"linux_signals":{"actions":[
    {"signal":11,"handler":"18446744073709551615","flags":"4294967296",
     "restorer":"9223372036854775808","mask":"9223372036854775809"}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxSignals);
  const auto &Action = O->LinuxSignals->Actions.at(11);
  EXPECT_EQ(Action.Handler, UINT64_MAX);
  EXPECT_EQ(Action.Flags, 0x100000000u);
  EXPECT_EQ(Action.Restorer, 0x8000000000000000);
  EXPECT_EQ(Action.Mask, 0x8000000000000001);
  for (auto P :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::LinuxSignalsProfile);
  }
  for (const auto &[Signal, Invalid] :
       std::map<int32_t, LinuxSignalAction>{{0, {}},
                                            {65, {}},
                                            {-1, {}},
                                            {11, {0, 0, 0, 0x100}},
                                            {9, {1, 0, 0, 0}}}) {
    O->LinuxSignals->Actions = {{Signal, Invalid}};
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("signal disposition"),
              std::string::npos);
  }
  auto Empty = processOptionsFromJSON(R"({"linux_signals":{"actions":[]}})");
  ASSERT_TRUE(bool(Empty));
  ASSERT_TRUE(Empty->LinuxSignals);
  EXPECT_TRUE(Empty->LinuxSignals->Actions.empty());
}

TEST(ProcessReport, MalformedSignalActionsFailBeforeExecution) {
  for (
      const char *Bad :
      {"null", "[]", "true", "{}", R"({"actions":{}})",
       R"({"actions":[],"unknown":0})",
       R"({"actions":[{"signal":11,"handler":0,"flags":0,"mask":0}]})",
       R"({"actions":[{"signal":11,"handler":0,"flags":0,"mask":0,"restorer":0,"unknown":0}]})",
       R"({"actions":[{"signal":11,"handler":0,"flags":0,"mask":0,"restorer":0},{"signal":11,"handler":1,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":4294967296,"handler":0,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":-1,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":9007199254740992,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":"18446744073709551616","flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":"0x1","flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":1.5,"flags":0,"mask":0,"restorer":0}]})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"linux_signals\":") + Bad + "}");
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

llvm::json::Value fileMetadata() {
  return llvm::cantFail(llvm::json::parse(R"({
    "device":4294967295,"inode":"18446744073709551615","mode":33279,
    "link_count":4294967295,"uid":4294967295,"gid":4294967295,
    "size":"9223372036854775807","block_size":2147483647,
    "blocks":"9223372036854775807",
    "access_time":{"seconds":"-9223372036854775808","nanoseconds":0},
    "modification_time":{"seconds":"9223372036854775807","nanoseconds":999999999},
    "change_time":{"seconds":-1,"nanoseconds":1}})"));
}
llvm::Expected<ProcessOptions> fileOptions(llvm::json::Value Metadata) {
  llvm::json::Object File{{"path", "/fixture/data"},
                          {"bytes_hex", "00ff"},
                          {"metadata", std::move(Metadata)}};
  llvm::json::Object Options{
      {"linux_files",
       llvm::json::Object{{"files", llvm::json::Array{std::move(File)}}}}};
  return processOptionsFromJSON(
      llvm::formatv("{0}", llvm::json::Value(std::move(Options))).str());
}

TEST(ProcessReport, FileMetadataPreservesFullWidthObservations) {
  auto O = fileOptions(fileMetadata());
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  const auto &M = O->LinuxFiles->Metadata.at("/fixture/data");
  EXPECT_EQ(M.Device, UINT32_MAX);
  EXPECT_EQ(M.Inode, UINT64_MAX);
  EXPECT_EQ(M.Mode, 0100777u);
  EXPECT_EQ(M.LinkCount, UINT32_MAX);
  EXPECT_EQ(M.UID, UINT32_MAX);
  EXPECT_EQ(M.GID, UINT32_MAX);
  EXPECT_EQ(M.Size, uint64_t(INT64_MAX));
  EXPECT_EQ(M.BlockSize, uint32_t(INT32_MAX));
  EXPECT_EQ(M.Blocks, uint64_t(INT64_MAX));
  EXPECT_EQ(M.AccessTime.Seconds, INT64_MIN);
  EXPECT_EQ(M.AccessTime.Nanoseconds, 0);
  EXPECT_EQ(M.ModificationTime.Seconds, INT64_MAX);
  EXPECT_EQ(M.ModificationTime.Nanoseconds, 999999999);
  EXPECT_EQ(M.ChangeTime.Seconds, -1);
  EXPECT_EQ(M.ChangeTime.Nanoseconds, 1);
}

TEST(ProcessReport, IncompleteAndMalformedFileMetadataFailsBeforeExecution) {
  auto Reject = [](llvm::json::Value Value) {
    auto O = fileOptions(std::move(Value));
    EXPECT_FALSE(bool(O));
    EXPECT_NE(llvm::toString(O.takeError()).find("linux_files"),
              std::string::npos);
  };
  for (const char *Text : {"null", "[]", "{}", "false", "0"})
    Reject(llvm::cantFail(llvm::json::parse(Text)));
  auto Good = fileMetadata();
  for (const auto &[Name, Value] : *Good.getAsObject()) {
    SCOPED_TRACE(Name.str());
    auto Missing = Good;
    Missing.getAsObject()->erase(Name);
    Reject(std::move(Missing));
    auto Bad = Good;
    (*Bad.getAsObject())[Name] = nullptr;
    Reject(std::move(Bad));
  }
  auto Extra = Good;
  (*Extra.getAsObject())["unknown"] = 0;
  Reject(std::move(Extra));
  for (const char *Name : {"device", "inode", "mode", "link_count", "uid",
                           "gid", "size", "block_size", "blocks"}) {
    for (const char *Text :
         {"-1", "1.5", "true", "\"\"", "\"+1\"", "\"-1\"", "\" 1\"", "\"0x1\"",
          "\"18446744073709551616\"", "9007199254740992"}) {
      SCOPED_TRACE(testing::Message() << Name << ':' << Text);
      auto Bad = Good;
      (*Bad.getAsObject())[Name] = llvm::cantFail(llvm::json::parse(Text));
      Reject(std::move(Bad));
    }
  }
  for (const auto &[Name, Text] : {std::pair{"device", "4294967296"},
                                   {"link_count", "4294967296"},
                                   {"uid", "4294967296"},
                                   {"gid", "4294967296"},
                                   {"mode", "16877"},
                                   {"mode", "98304"},
                                   {"block_size", "2147483648"},
                                   {"size", "\"9223372036854775808\""},
                                   {"blocks", "\"9223372036854775808\""}}) {
    SCOPED_TRACE(testing::Message() << Name << ':' << Text);
    auto Bad = Good;
    (*Bad.getAsObject())[Name] = llvm::cantFail(llvm::json::parse(Text));
    Reject(std::move(Bad));
  }
  for (const char *Name : {"access_time", "modification_time", "change_time"})
    for (const char *Text :
         {R"({"seconds":0})", R"({"seconds":0,"nanoseconds":0,"extra":0})",
          R"({"seconds":0,"nanoseconds":-1})",
          R"({"seconds":0,"nanoseconds":1000000000})",
          R"({"seconds":"9223372036854775808","nanoseconds":0})",
          R"({"seconds":"-9223372036854775809","nanoseconds":0})",
          R"({"seconds":1.5,"nanoseconds":0})",
          R"({"seconds":9007199254740992,"nanoseconds":0})"}) {
      SCOPED_TRACE(testing::Message() << Name << ':' << Text);
      auto Bad = Good;
      (*Bad.getAsObject())[Name] = llvm::cantFail(llvm::json::parse(Text));
      Reject(std::move(Bad));
    }
}

TEST(ProcessReport, DirectFileMetadataUsesTheSameValidationBoundary) {
  for (unsigned Mode = 0; Mode < 12; ++Mode) {
    SCOPED_TRACE(Mode);
    ProcessOptions Options;
    Options.LinuxFiles.emplace();
    auto &Files = *Options.LinuxFiles;
    Files.Files["/fixture/data"] = {1};
    auto &M = Files.Metadata[Mode ? "/fixture/data" : "/absent"];
    M = fileTestMetadata();
    if (Mode == 1)
      M.Mode = 0040755;
    if (Mode == 2)
      M.Mode |= 0x10000;
    if (Mode == 3)
      M.Size = UINT64_MAX;
    if (Mode == 4)
      M.Blocks = UINT64_MAX;
    if (Mode == 5)
      M.BlockSize = uint32_t(INT32_MAX) + 1;
    if (Mode >= 6) {
      LinuxTimespec *Times[] = {&M.AccessTime, &M.ModificationTime,
                                &M.ChangeTime};
      Times[(Mode - 6) / 2]->Nanoseconds = Mode % 2 ? 1000000000 : -1;
    }
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, Options);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
