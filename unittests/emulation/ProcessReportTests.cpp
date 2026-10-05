//===- ProcessReportTests.cpp - Lossless bounded process wire contract ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
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
TEST(ProcessReport, DarwinFileInputsAreLosslessAndRequireDarwinProfiles) {
  auto O = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/data","bytes_hex":"00FF78"}],"stdin_hex":"00ff",
    "descriptor_limit":32}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->DarwinFiles);
  EXPECT_EQ(O->DarwinFiles->Files.at("/data"),
            (std::vector<uint8_t>{0, 255, 'x'}));
  ASSERT_TRUE(O->DarwinFiles->StandardInput);
  EXPECT_EQ(*O->DarwinFiles->StandardInput, (std::vector<uint8_t>{0, 255}));
  EXPECT_EQ(O->DarwinFiles->DescriptorLimit, 32u);
  for (auto P : {ProcessProfile::LinuxELF64, ProcessProfile::WindowsPE64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinFilesProfile);
  }
  O->DarwinFiles->Files["/data/child"] = {};
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("catalogue path"),
              std::string::npos);
  }
  auto Empty = processOptionsFromJSON(R"({"darwin_files":{"files":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_FALSE(Empty->DarwinFiles->StandardInput);
  auto EOFInput =
      processOptionsFromJSON(R"({"darwin_files":{"files":[],"stdin_hex":""}})");
  ASSERT_TRUE(bool(EOFInput)) << llvm::toString(EOFInput.takeError());
  ASSERT_TRUE(EOFInput->DarwinFiles->StandardInput);
  EXPECT_TRUE(EOFInput->DarwinFiles->StandardInput->empty());
}
TEST(ProcessReport, DarwinDirectoriesAndWorkingDirectoryAreExplicitAndStrict) {
  auto O = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/empty/deep"}],"working_directory":"/empty"}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_EQ(O->DarwinFiles->WorkingDirectory, "/empty");
  EXPECT_EQ(O->DarwinFiles->Directories,
            (std::set<std::string>{"/empty/deep"}));
  auto M = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  (*M.getAsObject())["mode"] = 0040755;
  (*M.getAsObject())["size"] = "9223372036854775807";
  auto Directory = processOptionsFromJSON(
      R"({"darwin_files":{"files":[],"directories":[{"path":"/","metadata":)" +
      llvm::formatv("{0}", M).str() + R"(}],"working_directory":"/"}})");
  ASSERT_TRUE(bool(Directory)) << llvm::toString(Directory.takeError());
  EXPECT_EQ(Directory->DarwinFiles->Metadata.at("/").Size, uint64_t(INT64_MAX));
  for (
      auto Bad :
      {R"({"files":[],"directories":null})",
       R"({"files":[],"directories":["/empty"]})",
       R"({"files":[],"directories":[{"path":"/empty","extra":1}]})",
       R"({"files":[],"directories":[{"path":"/empty"},{"path":"/empty"}]})",
       R"({"files":[],"directories":[{"path":"/empty/"}]})",
       R"({"files":[{"path":"/a","bytes_hex":""}],"directories":[{"path":"/a"}]})",
       R"({"files":[{"path":"/a","bytes_hex":""}],"directories":[{"path":"/a/b"}]})",
       R"({"files":[],"working_directory":null})",
       R"({"files":[],"working_directory":""})",
       R"({"files":[],"working_directory":"relative"})",
       R"({"files":[],"working_directory":"/missing"})"}) {
    SCOPED_TRACE(Bad);
    auto Parsed =
        processOptionsFromJSON(std::string("{\"darwin_files\":") + Bad + '}');
    EXPECT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinDirectorySnapshotsHaveStrictLosslessWireFields) {
  const auto Original =
      llvm::cantFail(llvm::json::parse(darwin_test::DirectoryContentsJSON));
  auto Parse = [&](const llvm::json::Value &Contents) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[{"path":"/data","bytes_hex":""}],"directories":[{"path":"/empty"},{"path":"/","contents":)" +
        llvm::formatv("{0}", Contents).str() + "}]}}");
  };
  auto Good = Parse(Original);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->DirectoryContents.at("/").Entries[3].Inode,
            0xfedcba9876543210ULL);
  for (auto Field : {"inode", "type", "next_offset", "seek_offset", "name"}) {
    auto V = Original;
    auto *Entries = V.getAsObject()->getArray("entries");
    (*Entries)[0].getAsObject()->erase(Field);
    auto Bad = Parse(V);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (const char *BadValue : {"null", "[]", "-1", "1.5", "9007199254740992",
                               "\"18446744073709551616\""}) {
    auto V = Original;
    (*V.getAsObject()->getArray("entries"))[3].getAsObject()->operator[](
        "inode") = llvm::cantFail(llvm::json::parse(BadValue));
    auto Bad = Parse(V);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (auto Text :
       {"null", "[]", "{}", R"({"entries":[],"minimum_buffer_size":0})",
        R"({"entries":[],"minimum_buffer_size":1,"unknown":0})"}) {
    auto Bad = Parse(llvm::cantFail(llvm::json::parse(Text)));
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
}

TEST(ProcessReport, MalformedDarwinFilesFailBeforeExecution) {
  for (
      const char *Bad :
      {"null", "[]", "{}", R"({"files":{}})", R"({"files":[],"unknown":1})",
       R"({"files":[],"descriptor_limit":2})",
       R"({"files":[],"descriptor_limit":4097})",
       R"({"files":[],"descriptor_limit":4294967296})",
       R"({"files":[],"descriptor_limit":3.5})",
       R"({"files":[],"stdin_hex":null})", R"({"files":[],"stdin_hex":"0"})",
       R"({"files":[],"stdin_hex":"zz"})",
       R"({"files":[{"path":"/x","bytes_hex":"00","extra":1}]})",
       R"({"files":[{"path":"x","bytes_hex":""}]})",
       R"({"files":[{"path":"/x\u0000","bytes_hex":""}]})",
       R"({"files":[{"path":"/x/../y","bytes_hex":""}]})",
       R"({"files":[{"path":"/x/","bytes_hex":""}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x","bytes_hex":"00"}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x/y","bytes_hex":""}]})"}) {
    SCOPED_TRACE(Bad);
    auto O =
        processOptionsFromJSON(std::string("{\"darwin_files\":") + Bad + '}');
    EXPECT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  ProcessOptions O;
  O.DarwinFiles.emplace();
  O.DarwinFiles->StandardInput =
      std::vector<uint8_t>(darwin_file_limits::Bytes + 1);
  auto R = emulateProcess("missing", ProcessProfile::MacOSMachO64, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("file input limits"),
            std::string::npos);
}

TEST(ProcessReport, DarwinMetadataIntegersAreLosslessAndStrictlyAdmitted) {
  const auto Original =
      llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  auto Parse = [&](const llvm::json::Value &Metadata) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":)" +
        llvm::formatv("{0}", Metadata).str() + "}]}}");
  };
  auto Good = Parse(Original);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &M = Good->DarwinFiles->Metadata.at("/data");
  EXPECT_EQ(M.Device, -123);
  EXPECT_EQ(M.Inode, 0xfedcba9876543210ULL);
  EXPECT_EQ(M.Mode, 0100644);
  EXPECT_EQ(M.UID, 0x89abcdefu);
  EXPECT_EQ(M.GID, 0xfedcba98u);
  EXPECT_EQ(M.Generation, 0x89abcdefu);
  EXPECT_EQ(M.AccessTime.Seconds, INT64_MIN + 1);
  EXPECT_EQ(M.ModificationTime.Seconds, INT64_MAX);
  EXPECT_EQ(M.ModificationTime.Nanoseconds, 999999999);
  EXPECT_EQ(M.BirthTime.Seconds, -5);
  EXPECT_EQ(M.BirthTime.Nanoseconds, 6);
  const std::pair<llvm::StringRef, llvm::json::Value> Invalid[] = {
      {"device", uint64_t(2147483648)},
      {"mode", 65536},
      {"mode", 0040644},
      {"link_count", 65536},
      {"uid", -1},
      {"gid", uint64_t(4294967296)},
      {"inode", 9007199254740992.0},
      {"inode", "18446744073709551616"},
      {"size", 9},
      {"blocks", "9223372036854775808"},
      {"block_size", uint64_t(2147483648)},
      {"flags", true},
      {"generation", 1.25},
      {"birth_time", nullptr},
      {"unknown", 0}};
  for (const auto &[Name, Value] : Invalid) {
    SCOPED_TRACE(Name.str());
    auto Changed = Original;
    (*Changed.getAsObject())[Name] = Value;
    auto Bad = Parse(Changed);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (const auto &[Name, Value] : *Original.getAsObject()) {
    auto Missing = Original;
    Missing.getAsObject()->erase(Name);
    auto Bad = Parse(Missing);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (auto Time :
       {"access_time", "modification_time", "change_time", "birth_time"})
    for (auto Number : {-1, 1000000000}) {
      auto Changed = Original;
      (*Changed.getAsObject()->getObject(Time))["nanoseconds"] = Number;
      auto Bad = Parse(Changed);
      EXPECT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
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
