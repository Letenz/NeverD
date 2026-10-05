//===- ProcessPublicTests.cpp - Real ELF through the shared SDK and CLI --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionReportFields.h"
#include "neverd/emulation/ProcessCLIStrings.h"
#include "neverd/emulation/ProcessReportFields.h"
#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

namespace {
using namespace neverd;
namespace field = emulation::process_report;
namespace cpu_field = emulation::execution_report;
#define NEVERD_EXECUTION_AVAILABILITY(Name, Text) constexpr char Name[] = Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_AVAILABILITY
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_LINUX_FIXTURE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#define NEVERD_LINUX_FIXTURE_MODE(Name, Character, Text)                       \
  constexpr char Name[] = Text;
#include "fixtures/LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_MODE
#undef NEVERD_LINUX_FIXTURE_TEXT
#undef NEVERD_LINUX_FIXTURE_VALUE
#define NEVERD_PROCESS_PROFILE(Name, Text) constexpr char Name[] = Text;
#include "neverd/emulation/ProcessProfile.def"
#undef NEVERD_PROCESS_PROFILE
namespace tls_fixture {
#define NEVERD_TLS_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_TLS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxTLSCases.def"
#undef NEVERD_TLS_VALUE
#undef NEVERD_TLS_TEXT
} // namespace tls_fixture
namespace pie_fixture {
#define NEVERD_PIE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PIE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxPIECases.def"
#undef NEVERD_PIE_VALUE
#undef NEVERD_PIE_TEXT
} // namespace pie_fixture
namespace memory_fixture {
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_MEMORY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
#undef NEVERD_LINUX_MEMORY_TEXT
} // namespace memory_fixture

namespace windows_fixture {
#define NEVERD_WINDOWS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "WindowsProcessTestData.def"
#undef NEVERD_WINDOWS_TEST_TEXT
#define NEVERD_WINDOWS_FIXTURE_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#define NEVERD_WINDOWS_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsProcessCases.def"
#undef NEVERD_WINDOWS_FIXTURE_TEXT
#undef NEVERD_WINDOWS_FIXTURE_VALUE
} // namespace windows_fixture
namespace module_fixture {
#define NEVERD_MODULE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MODULE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsModuleCases.def"
#undef NEVERD_MODULE_TEXT
#undef NEVERD_MODULE_VALUE
} // namespace module_fixture
namespace export_fixture {
#define NEVERD_EXPORT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_EXPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsExportCases.def"
#undef NEVERD_EXPORT_TEXT
#undef NEVERD_EXPORT_VALUE
} // namespace export_fixture
std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Copy(Value);
  neverd_free_string(Value);
  return Copy;
}
std::string jsonText(llvm::json::Object Value) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(std::move(Value));
  return Text;
}

class ProcessPublic : public testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::string Path;
  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    Path =
        (std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) / ARMFile).string();
    // This portable public-surface suite is separate from the process tests'
    // six transport/ISA cells; it must not pull a second engine into the DSO.
    auto Text =
        takeString(neverd_cpu_capabilities_json(Session, ProbeOptions, 1));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Probe = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    const auto *Host = Probe->getAsObject()->getObject(cpu_field::Host);
    ASSERT_NE(Host, nullptr);
    if (Host->getString(cpu_field::Availability) != Available)
      GTEST_SKIP() << Host->getString(cpu_field::Reason)->str();
#endif
  }
  void TearDown() override { neverd_session_destroy(Session); }
  std::string options(const char *Mode) {
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    Request[field::Arguments] = llvm::json::Array{ExecutableName, Mode};
    Request[field::Environment] = llvm::json::Array{Environment};
    Request[field::InstructionLimit] = 1000;
    return jsonText(std::move(Request));
  }
  llvm::json::Value run(const std::string &Options) {
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), LinuxELF64, Options.c_str()));
    EXPECT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    return llvm::cantFail(llvm::json::parse(Text));
  }
};

TEST_F(ProcessPublic, ReturnsOutputAndGuestStatusWithoutLoadingAnalysisImage) {
  for (const char *File : {X64File, ARMFile}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(Path).parent_path() / File).string();
    auto Result = run(options(Normal));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::Version),
              NEVERD_PROCESS_SCHEMA_VERSION);
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus), ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stderr), BinaryHex);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  }
  ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1)
      << takeString(neverd_last_error(Session));
  const auto Before = takeString(neverd_segments_json(Session));
  run(options(Normal));
  EXPECT_EQ(neverd_session_is_loaded(Session), 1);
  EXPECT_EQ(takeString(neverd_segments_json(Session)), Before);
}

TEST_F(ProcessPublic, WindowsPE64RunsThroughSDKAndCLIWithoutAnalysisState) {
#ifndef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << windows_fixture::MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto *File :
       {windows_fixture::X64File, windows_fixture::AArch64File}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) / File)
               .string();
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    Request[field::Arguments] =
        llvm::json::Array{windows_fixture::Executable, windows_fixture::Normal};
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), WindowsPE64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Result = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              windows_fixture::ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(std::string(windows_fixture::Message) +
                              windows_fixture::Detached,
                          true));
    ASSERT_NE(Result.getAsObject()->getObject(field::Windows), nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" +
                         WindowsPE64 + " --" + process_cli::OptionsOption +
                         "=" + test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::GuestFailure);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Result);
  }
#endif
}

TEST_F(ProcessPublic, WindowsStartupDLLsUseTheSameCatalogueThroughSDKAndCLI) {
#ifndef NEVERD_WINDOWS_MODULE_FIXTURE_DIR
  GTEST_SKIP() << module_fixture::MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto *File :
       {module_fixture::X64Dir, module_fixture::AArch64Dir}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_WINDOWS_MODULE_FIXTURE_DIR) / File /
            module_fixture::ProgramFile)
               .string();
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    llvm::json::Array Modules;
    for (const char *Name :
         {module_fixture::LeafFile, module_fixture::MiddleFile})
      Modules.push_back(llvm::json::Object{
          {field::Name, Name},
          {field::Path,
           (std::filesystem::path(NEVERD_WINDOWS_MODULE_FIXTURE_DIR) / File /
            Name)
               .string()}});
    Request[field::Windows] =
        llvm::json::Object{{field::Modules, std::move(Modules)}};
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), WindowsPE64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Result = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              module_fixture::ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(std::string(module_fixture::Message) +
                              module_fixture::Detached,
                          true));
    ASSERT_NE(Result.getAsObject()->getObject(field::Windows), nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" +
                         WindowsPE64 + " --" + process_cli::OptionsOption +
                         "=" + test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::GuestFailure);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Result);
  }
#endif
}

TEST_F(ProcessPublic, WindowsRuntimeExportsAgreeAcrossSDKAndCLI) {
#ifndef NEVERD_WINDOWS_EXPORT_FIXTURE_DIR
  GTEST_SKIP() << export_fixture::MissingTools;
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto *File :
       {export_fixture::X64Dir, export_fixture::AArch64Dir}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / File /
            export_fixture::ProgramFile)
               .string();
    llvm::json::Object Request;
    Request[field::Backend] = emulation::execution::Unicorn;
    Request[field::Arguments] = llvm::json::Array{
        export_fixture::ProgramFile, export_fixture::NormalArgument};
    llvm::json::Array Modules;
    for (const char *Name :
         {export_fixture::LeafFile, export_fixture::BridgeFile,
          export_fixture::TopFile})
      Modules.push_back(llvm::json::Object{
          {field::Name, Name},
          {field::Path,
           (std::filesystem::path(NEVERD_WINDOWS_EXPORT_FIXTURE_DIR) / File /
            Name)
               .string()}});
    Request[field::Windows] =
        llvm::json::Object{{field::Modules, std::move(Modules)}};
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), WindowsPE64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Result = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              export_fixture::ExitStatus);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(export_fixture::NormalTrace, true));
    ASSERT_NE(Result.getAsObject()->getObject(field::Windows), nullptr);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" +
                         WindowsPE64 + " --" + process_cli::OptionsOption +
                         "=" + test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::GuestFailure);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Result);
  }
#endif
}

TEST_F(ProcessPublic, DarwinProfilesPreserveBSDResultsAcrossSDKAndCLI) {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
  GTEST_SKIP() << "Clang and ld64.lld Darwin fixtures unavailable";
#else
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const auto &[File, Profile] :
       {std::pair{"macos-arm64", MacOSMachO64},
        std::pair{"macos-x86_64", MacOSMachO64},
        std::pair{"ios-arm64", IOSMachO64},
        std::pair{"ios-simulator-arm64", IOSSimulatorMachO64},
        std::pair{"ios-simulator-x86_64", IOSSimulatorMachO64}}) {
    SCOPED_TRACE(File);
    Path = (std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) / File).string();
    const std::string Options =
        R"({"backend":"unicorn","arguments":["guest","normal","argument"],"environment":["MODE=test"]})";
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), Profile, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    ASSERT_NE(Report->getAsObject(), nullptr);
    EXPECT_EQ(Report->getAsObject()->getString(field::Profile), Profile);
    EXPECT_EQ(Report->getAsObject()->getInteger(field::ExitStatus), 37);
    EXPECT_EQ(Report->getAsObject()->getString(field::Stdout),
              "64617277696e00ff0a");
    auto *Services = Report->getAsObject()->getArray(field::Services);
    ASSERT_NE(Services, nullptr);
    ASSERT_EQ(Services->size(), 4u);
    EXPECT_EQ((*Services)[0].getAsObject()->getBoolean(field::Error), true);
    EXPECT_EQ((*Services)[1].getAsObject()->getBoolean(field::Error), false);
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" + Profile +
                         " --" + process_cli::OptionsOption + "=" +
                         test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::GuestFailure);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              *Report);
    auto Wrong = takeString(neverd_emulate_process_json(
        Session, Path.c_str(),
        File == std::string("ios-arm64") ? MacOSMachO64 : IOSMachO64,
        Options.c_str()));
    EXPECT_TRUE(Wrong.empty());
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
  }
#endif
}

TEST_F(ProcessPublic, AndroidNativeFunctionReturnsThroughSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "android.so")
             .string();
  for (
      const auto &[Request, Expected] :
      std::vector<std::pair<std::string, std::string>>{
          {R"({"backend":"unicorn","android":{"entry_symbol":"add_arguments","arguments":[1,2,3,4,5,6,7,8,9,10],"trace_limit":4096}})",
           "192"},
          {R"({"backend":"unicorn","android":{"entry_symbol":"dynamic_lookup","libraries":{"libfixture.so":["strlen"]}}})",
           "4"},
          {R"({"backend":"unicorn","android":{"entry_symbol":"default_call","libraries":{"libfixture.so":["strlen"]},"default_scope":["libfixture.so"]}})",
           "6"},
          {R"({"backend":"unicorn","android":{"entry_symbol":"dynamic_identities","arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":16}],"libraries":{"libidentity.so":["getuid","geteuid","getgid","getegid"]}}})",
           "0"}}) {
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
    EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), Expected);
    if (Expected == "4" || Expected == "6") {
      bool NamedLookup = false;
      const auto *Android = Parsed.getAsObject()->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      for (const auto &Event : *Android->getArray(field::NativeCalls)) {
        const auto *E = Event.getAsObject();
        if (E->getString(field::Name) == "dlsym") {
          EXPECT_EQ(E->getString(field::Library), "libfixture.so");
          EXPECT_EQ(E->getString(field::Symbol), "strlen");
          NamedLookup = true;
        }
      }
      EXPECT_TRUE(NamedLookup);
    }
    if (Expected == "0") {
      const auto *Android = Parsed.getAsObject()->getObject(field::Android);
      ASSERT_NE(Android, nullptr);
      for (const char *Name : {"getuid", "geteuid", "getgid", "getegid"}) {
        const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
        for (const auto &Event : *Android->getArray(field::NativeCalls)) {
          const auto *E = Event.getAsObject();
          if (E->getString(field::Name) == "dlsym" &&
              E->getString(field::Symbol) == Name)
            Lookup = E;
          if (E->getString(field::Name) == Name)
            Call = E;
        }
        ASSERT_NE(Lookup, nullptr);
        ASSERT_NE(Call, nullptr);
        EXPECT_EQ(Lookup->getString(field::Library), "libidentity.so");
        EXPECT_EQ(Call->getString(field::Library), "libidentity.so");
        EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
        EXPECT_EQ(Call->getString(field::Result), "3e8");
      }
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
    const std::filesystem::path Root(Directory.str().str());
    auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
    const auto Output = (Root / OutputFile).string();
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                         test::shellQuote(Path) +
                         " --profile=" + AndroidNativeAArch64 +
                         " --options=" + test::shellQuote(Request) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::Success);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Parsed);
  }
#endif
}

TEST_F(ProcessPublic, AndroidFinalizersKeepGuestEffectsAcrossSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
          "finalizers-O2-relr.so")
             .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"cxa_dynamic","arguments":["0x20000000",0],"initialize":false,"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":256}],"libraries":{"libfinalize-model.so":["__cxa_atexit","__cxa_finalize"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "49");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *E = Event.getAsObject();
    if (E->getString(field::Name) == "dlsym")
      Lookup = E;
    if (E->getString(field::Name) == "__cxa_finalize")
      Call = E;
  }
  ASSERT_NE(Lookup, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Lookup->getString(field::Symbol), "__cxa_finalize");
  EXPECT_EQ(Call->getString(field::Library), "libfinalize-model.so");
  EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
  EXPECT_EQ(Call->getString(field::Result), "0");
  std::vector<uint8_t> Expected(256);
  llvm::support::endian::write64le(Expected.data(), 1);
  llvm::support::endian::write64le(Expected.data() + 48, 1000);
  llvm::support::endian::write64le(Expected.data() + 64, 0x100000009);
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  EXPECT_EQ(Memory->front().getAsObject()->getString(field::Bytes),
            llvm::toHex(Expected, true));
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidFormattingKeepsDynamicNamesAcrossSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "format-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"format_dynamic","arguments":["0x20000000",0],"initialize":false,"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":64}],"libraries":{"libformat-model.so":["snprintf"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "12");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *E = Event.getAsObject();
    if (E->getString(field::Name) == "dlsym")
      Lookup = E;
    if (E->getString(field::Name) == "snprintf")
      Call = E;
  }
  ASSERT_NE(Lookup, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Lookup->getString(field::Symbol), "snprintf");
  EXPECT_EQ(Call->getString(field::Library), "libformat-model.so");
  EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
  EXPECT_EQ(Call->getString(field::Result), "12");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidMemoryFilesShareStateThroughCAPIAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "files-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Entry :
       {"files_sequence", "files_faults", "files_bionic", "files_status",
        "files_status_bionic", "files_access", "files_access_faults",
        "files_access_bionic", "files_directory_errors",
        "files_directory_bionic"}) {
    SCOPED_TRACE(Entry);
    llvm::json::Object Request{
        {"backend", "unicorn"},
        {"instruction_quantum", 31},
        {"android", llvm::json::Object{{"entry_symbol", Entry},
                                       {"initialize", false},
                                       {"thread_limit", 2}}},
        {"linux_files",
         llvm::json::Object{{"files", llvm::json::Array{llvm::json::Object{
                                          {"path", "/fixture/data"},
                                          {"bytes_hex", "00ff410a805a"}}}}}}};
    if (llvm::StringRef(Entry).starts_with("files_status"))
      (*Request.getObject("linux_files")
            ->getArray("files")
            ->front()
            .getAsObject())["metadata"] =
          llvm::cantFail(llvm::json::parse(emulation::FileTestMetadataJSON));
    if (llvm::StringRef(Entry) == "files_access" ||
        llvm::StringRef(Entry) == "files_directory_errors")
      (*Request.getObject("linux_files"))["descriptor_limit"] = 4;
    auto Options = jsonText(std::move(Request));
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Options.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Report = llvm::cantFail(llvm::json::parse(Text));
    EXPECT_EQ(Report.getAsObject()->getString(field::Stop), "returned");
    EXPECT_EQ(Report.getAsObject()->getString(field::ReturnValue), "0");
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                         test::shellQuote(Path) +
                         " --profile=" + AndroidNativeAArch64 +
                         " --options=" + test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::Success);
    auto Bytes = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Bytes));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Report);
  }
#endif
}

TEST_F(ProcessPublic, AndroidLocalMemoryInputMatchesSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "android.so")
             .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / "input.bin").string();
  std::vector<uint8_t> Bytes(65539);
  uint64_t Hash = 14695981039346656037ull;
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = static_cast<uint8_t>(I * 37 + (I >> 8));
  for (size_t I = 0; I < Bytes.size();) {
    uint64_t Word = 0;
    size_t Width = Bytes.size() - I >= 8 ? 8 : 1;
    for (size_t J = 0; J < Width; ++J)
      Word |= uint64_t(Bytes[I++]) << (8 * J);
    Hash = (Hash ^ Word) * 1099511628211ull;
  }
  {
    std::error_code E;
    llvm::raw_fd_ostream OS(Input, E);
    ASSERT_FALSE(E);
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
  }
  llvm::json::Object Options{
      {field::Backend, "unicorn"},
      {field::InstructionLimit, 1000000},
      {field::Timeout, 20000000},
      {field::Android,
       llvm::json::Object{
           {field::EntrySymbol, "inspect_memory_input"},
           {field::Arguments, llvm::json::Array{0x20000000, Bytes.size()}},
           {field::Memory,
            llvm::json::Array{llvm::json::Object{{field::Address, 0x20000000},
                                                 {field::Size, 18 * 4096},
                                                 {field::Path, Input}}}},
           {field::ReadMemory,
            llvm::json::Array{llvm::json::Object{{field::Address, 0x20000000},
                                                 {field::Size, 18 * 4096}}}}}}};
  std::string Request;
  llvm::raw_string_ostream(Request) << llvm::json::Value(std::move(Options));
  ASSERT_LT(Request.size(), field::JSONLimit);
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue),
            llvm::utohexstr(Hash, true));
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  auto Mutated = Bytes;
  Mutated[0] ^= 255;
  Mutated.push_back(165);
  Mutated.resize(18 * 4096, 0);
  ASSERT_EQ(Android->getArray(field::Memory)->size(), 1u);
  EXPECT_EQ(Android->getArray(field::Memory)
                ->front()
                .getAsObject()
                ->getString(field::Bytes),
            llvm::toHex(Mutated, true));
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Report = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Report)->getBuffer())), Parsed);
  auto Original = llvm::MemoryBuffer::getFile(Input);
  ASSERT_TRUE(bool(Original));
  EXPECT_EQ((*Original)->getBuffer(),
            llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                            Bytes.size()));
#endif
}

TEST_F(ProcessPublic, AndroidInitializersReturnThroughSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "once-O2-relr.so")
             .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"once_values","arguments":["0x20000000"],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":44}]}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "49");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const auto *Calls = Android->getArray(field::NativeCalls);
  ASSERT_NE(Calls, nullptr);
  ASSERT_EQ(Calls->size(), 6u);
  for (size_t I = 0; I < Calls->size(); ++I) {
    EXPECT_EQ((*Calls)[I].getAsObject()->getString(field::Name),
              I == 4 ? "getuid" : "pthread_once");
    EXPECT_EQ((*Calls)[I].getAsObject()->getString(field::Result),
              I == 4 ? "3e8" : "0");
  }
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  EXPECT_EQ((*Memory)[0].getAsObject()->getString(field::Bytes),
            "02000000020000000100000001000000010000000100000002000000"
            "e8030000010000000200000001000000");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Buffer = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Buffer));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidFortifiedSearchReportsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "search-O2-relr.so")
          .string();
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (bool Fails : {false, true}) {
    const std::string Request =
        Fails
            ? R"({"backend":"unicorn","android":{"entry_symbol":"search_supplied","initialize":false,"arguments":[1,58,0,2]}})"
            : R"({"backend":"unicorn","android":{"entry_symbol":"search_dynamic","initialize":false,"arguments":["0x20000000",0,0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":24}],"libraries":{"libsearch.so":["__strchr_chk"]}}})";
    auto Text = takeString(neverd_emulate_process_json(
        Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
    ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
    auto Parsed = llvm::cantFail(llvm::json::parse(Text));
    const auto *Report = Parsed.getAsObject();
    ASSERT_NE(Report, nullptr);
    ASSERT_EQ(Report->getString(field::Stop),
              Fails ? "runtime_failure" : "returned");
    const auto *Android = Report->getObject(field::Android);
    ASSERT_NE(Android, nullptr);
    const auto *Calls = Android->getArray(field::NativeCalls);
    ASSERT_NE(Calls, nullptr);
    ASSERT_FALSE(Calls->empty());
    if (Fails) {
      EXPECT_NE(Report->getString(field::Diagnostic)->find("FORTIFY"),
                llvm::StringRef::npos);
      const auto *Last = Calls->back().getAsObject();
      EXPECT_EQ(Last->getString(field::Name), "__strchr_chk");
      EXPECT_EQ(*Last->get(field::Result), llvm::json::Value(nullptr));
    } else {
      EXPECT_EQ(Report->getString(field::ReturnValue), "0");
      const llvm::json::Object *Lookup = nullptr;
      unsigned Searches = 0;
      for (const auto &Event : *Calls) {
        const auto *Call = Event.getAsObject();
        if (Call->getString(field::Name) == "dlsym") {
          Lookup = Call;
          EXPECT_EQ(Call->getString(field::Symbol), "__strchr_chk");
        }
        if (Call->getString(field::Name) == "__strchr_chk") {
          ASSERT_NE(Lookup, nullptr);
          EXPECT_EQ(Call->getString(field::Library), "libsearch.so");
          EXPECT_EQ(Call->getString(field::PC),
                    Lookup->getString(field::Result));
          ++Searches;
        }
      }
      EXPECT_EQ(Searches, 2u);
      const auto *Memory = Android->getArray(field::Memory);
      ASSERT_NE(Memory, nullptr);
      ASSERT_EQ(Memory->size(), 1u);
      EXPECT_EQ((*Memory)[0].getAsObject()->getString(field::Bytes),
                "04000000000000000f00000000000000dd02000000000000");
    }
    EXPECT_EQ(neverd_session_is_loaded(Session), 0);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                         test::shellQuote(Path) +
                         " --profile=" + AndroidNativeAArch64 +
                         " --options=" + test::shellQuote(Request) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              Fails ? process_cli::Incomplete : process_cli::Success);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Buffer)->getBuffer())),
              Parsed);
  }
#endif
}

TEST_F(ProcessPublic, AndroidTokenNamesAndMutationsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "token-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"token_dynamic","initialize":false,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":40}],"libraries":{"libtokens.so":["strtok_r"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const auto *Calls = Android->getArray(field::NativeCalls);
  ASSERT_NE(Calls, nullptr);
  const llvm::json::Object *Lookup = nullptr;
  unsigned Tokens = 0;
  for (const auto &Event : *Calls) {
    const auto *Call = Event.getAsObject();
    if (Call->getString(field::Name) == "dlsym") {
      Lookup = Call;
      EXPECT_EQ(Call->getString(field::Symbol), "strtok_r");
      EXPECT_EQ(Call->getString(field::Library), "libtokens.so");
    }
    if (Call->getString(field::Name) == "strtok_r") {
      ASSERT_NE(Lookup, nullptr);
      EXPECT_EQ(Call->getString(field::Library), "libtokens.so");
      EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
      ++Tokens;
    }
  }
  EXPECT_EQ(Tokens, 2u);
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  EXPECT_EQ((*Memory)[0].getAsObject()->getString(field::Bytes),
            "000000000000000006000000000000000000000000000000"
            "06000000000000000000000000000000");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Report = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Report));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Report)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, ExplicitClockValuesAndNamesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "time-O2-relr.so")
             .string();
  const std::string Request = R"({"backend":"unicorn","linux_time":{
    "clocks":[{"id":0,"seconds":"4294967297","nanoseconds":987654321},
              {"id":1,"seconds":123,"nanoseconds":456789}]},
    "android":{"entry_symbol":"time_dynamic","initialize":false,
      "arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],
      "read_memory":[{"address":"0x20000000","size":40}],
      "libraries":{"libclock-model.so":["time","clock_gettime","gettimeofday"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  ASSERT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  for (const char *Name : {"time", "clock_gettime", "gettimeofday"}) {
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Android->getArray(field::NativeCalls)) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym" &&
          E->getString(field::Symbol) == Name)
        Lookup = E;
      if (E->getString(field::Name) == Name)
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString(field::Library), "libclock-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    EXPECT_EQ(Call->getString(field::Result),
              std::string(Name) == "time" ? "100000001" : "0");
  }
  EXPECT_EQ(Android->getArray(field::Memory)
                ->front()
                .getAsObject()
                ->getString(field::Bytes),
            "01000000010000007b0000000000000055f8060000000000010000000100000006"
            "120f0000000000");
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidMutexNamesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "mutex-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"mutex_dynamic","initialize":false,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":24}],"libraries":{"libpthread-model.so":["pthread_mutex_lock","pthread_mutex_unlock"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  for (const char *Name : {"pthread_mutex_lock", "pthread_mutex_unlock"}) {
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Android->getArray(field::NativeCalls)) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym" &&
          E->getString(field::Symbol) == Name)
        Lookup = E;
      if (E->getString(field::Name) == Name)
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString(field::Library), "libpthread-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    EXPECT_EQ(Call->getString(field::Result), "0");
  }
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidGuestThreadsMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "threads-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","instruction_quantum":7,"android":{"entry_symbol":"threads_dynamic","thread_limit":4,"initialize":false,"trace_limit":1000,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":64}],"libraries":{"libthread-model.so":["pthread_create","pthread_join"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "4b");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const auto *Threads = Android->getArray(field::Threads);
  ASSERT_NE(Threads, nullptr);
  ASSERT_EQ(Threads->size(), 2u);
  EXPECT_EQ((*Threads)[1].getAsObject()->getBoolean(field::Finished), true);
  EXPECT_EQ((*Threads)[1].getAsObject()->getBoolean(field::Retired), true);
  ASSERT_NE(Android->getArray(field::TraceThreads), nullptr);
  EXPECT_GT(Android->getArray(field::TraceThreads)->size(), 1u);
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *Call = Event.getAsObject();
    if (Call->getString(field::Name) == "pthread_create" ||
        Call->getString(field::Name) == "pthread_join") {
      EXPECT_EQ(Call->getString(field::Library), "libthread-model.so");
      EXPECT_EQ(Call->getInteger(field::ThreadID), 1000);
      EXPECT_EQ(Call->getString(field::Result), "0");
    }
  }
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidThreadAttributesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path = (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
          "thread-attributes-O2-relr.so")
             .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"thread_attribute_dynamic","initialize":false,"arguments":["0x20000000",0],"memory":[{"address":"0x20000000","size":4096}],"read_memory":[{"address":"0x20000000","size":80}],"libraries":{"libthread-model.so":["pthread_attr_init","pthread_attr_getstacksize"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  for (const char *Name : {"pthread_attr_init", "pthread_attr_getstacksize"}) {
    const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
    for (const auto &Event : *Android->getArray(field::NativeCalls)) {
      const auto *E = Event.getAsObject();
      if (E->getString(field::Name) == "dlsym" &&
          E->getString(field::Symbol) == Name)
        Lookup = E;
      if (E->getString(field::Name) == Name)
        Call = E;
    }
    ASSERT_NE(Lookup, nullptr);
    ASSERT_NE(Call, nullptr);
    EXPECT_EQ(Call->getString(field::Library), "libthread-model.so");
    EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
    EXPECT_EQ(Call->getString(field::Result), "0");
  }
  const auto *Memory = Android->getArray(field::Memory);
  ASSERT_NE(Memory, nullptr);
  ASSERT_EQ(Memory->size(), 1u);
  auto Hex = Memory->front().getAsObject()->getString(field::Bytes);
  ASSERT_TRUE(Hex);
  EXPECT_EQ(Hex->substr(32, 16), "00c00f0000000000");
  EXPECT_EQ(Hex->substr(56, 8), "a5a5a5a5");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, AndroidSyscallNamesMatchSDKAndCLI) {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
  GTEST_SKIP() << "Android shared library fixtures unavailable";
#else
  Path =
      (std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) / "syscall-O2-relr.so")
          .string();
  const std::string Request =
      R"({"backend":"unicorn","android":{"entry_symbol":"syscall_dynamic","initialize":false,"arguments":[0],"libraries":{"libservice.so":["syscall"]}}})";
  auto Text = takeString(neverd_emulate_process_json(
      Session, Path.c_str(), AndroidNativeAArch64, Request.c_str()));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Parsed = llvm::cantFail(llvm::json::parse(Text));
  EXPECT_EQ(Parsed.getAsObject()->getString(field::Stop), "returned");
  EXPECT_EQ(Parsed.getAsObject()->getString(field::ReturnValue), "0");
  const auto *Android = Parsed.getAsObject()->getObject(field::Android);
  ASSERT_NE(Android, nullptr);
  const llvm::json::Object *Lookup = nullptr, *Call = nullptr;
  for (const auto &Event : *Android->getArray(field::NativeCalls)) {
    const auto *E = Event.getAsObject();
    if (E->getString(field::Name) == "dlsym" &&
        E->getString(field::Symbol) == "syscall")
      Lookup = E;
    if (E->getString(field::Name) == "syscall")
      Call = E;
  }
  ASSERT_NE(Lookup, nullptr);
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->getString(field::Library), "libservice.so");
  EXPECT_EQ(Call->getString(field::PC), Lookup->getString(field::Result));
  EXPECT_EQ(Call->getString(field::Result), "3e8");
  EXPECT_EQ((*Call->getArray(field::Arguments))[0].getAsString(), "b2");
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " emulate " +
                       test::shellQuote(Path) +
                       " --profile=" + AndroidNativeAArch64 +
                       " --options=" + test::shellQuote(Request) +
                       test::redirectStdout(Output) + test::silenceStderr();
  EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
            process_cli::Success);
  auto Bytes = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Bytes));
  EXPECT_EQ(llvm::cantFail(llvm::json::parse((*Bytes)->getBuffer())), Parsed);
#endif
}

TEST_F(ProcessPublic, InvalidSetupDoesNotPoisonTheReusableSession) {
  EXPECT_EQ(
      neverd_emulate_process_json(nullptr, Path.c_str(), LinuxELF64, nullptr),
      nullptr);
  EXPECT_EQ(neverd_emulate_process_json(Session, nullptr, LinuxELF64, nullptr),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::PathRequired);
  EXPECT_EQ(
      neverd_emulate_process_json(Session, Path.c_str(), nullptr, nullptr),
      nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::ProfileRequired);
  EXPECT_EQ(
      neverd_emulate_process_json(Session, MissingPath, LinuxELF64, nullptr),
      nullptr);
  EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());
  EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(), UnknownProfile,
                                        nullptr),
            nullptr);
#define NEVERD_PROCESS_INVALID_JSON(Name, Text)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    EXPECT_EQ(                                                                 \
        neverd_emulate_process_json(Session, Path.c_str(), LinuxELF64, Text),  \
        nullptr);                                                              \
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());              \
  }
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_INVALID_JSON
  const std::string Large(NEVERD_PROCESS_OPTIONS_JSON_LIMIT + 1, ' ');
  EXPECT_EQ(neverd_emulate_process_json(Session, Path.c_str(), LinuxELF64,
                                        Large.c_str()),
            nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), field::TooLarge);
  run(options(Normal));
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST_F(ProcessPublic, CompilerStartupRunsThroughTheSharedSDKAndCLI) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  struct Fixture {
    const char *File, *Message;
    uint64_t Limit, Status;
  };
  const Fixture Fixtures[] = {
      {tls_fixture::X64File, tls_fixture::Message,
       tls_fixture::InstructionLimit, tls_fixture::ExitStatus},
      {tls_fixture::ARMFile, tls_fixture::Message,
       tls_fixture::InstructionLimit, tls_fixture::ExitStatus},
      {pie_fixture::X64File, pie_fixture::Message,
       pie_fixture::InstructionLimit, pie_fixture::ExitStatus},
      {pie_fixture::ARMFile, pie_fixture::Message,
       pie_fixture::InstructionLimit, pie_fixture::ExitStatus},
      {memory_fixture::X64File, memory_fixture::Message,
       memory_fixture::InstructionLimit, memory_fixture::ExitStatus},
      {memory_fixture::ARMFile, memory_fixture::Message,
       memory_fixture::InstructionLimit, memory_fixture::ExitStatus}};
  for (const auto &Fixture : Fixtures) {
    SCOPED_TRACE(Fixture.File);
    Path = (std::filesystem::path(Path).parent_path() / Fixture.File).string();
    auto Request = llvm::cantFail(llvm::json::parse(options(Normal)));
    (*Request.getAsObject())[field::InstructionLimit] = Fixture.Limit;
    const auto Options = jsonText(std::move(*Request.getAsObject()));
    const auto Result = run(Options);
    EXPECT_EQ(Result.getAsObject()->getInteger(field::ExitStatus),
              Fixture.Status);
    EXPECT_EQ(Result.getAsObject()->getString(field::Stdout),
              llvm::toHex(Fixture.Message, true));
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" + LinuxELF64 +
                         " --" + process_cli::OptionsOption + "=" +
                         test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              process_cli::Success);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    auto Report = llvm::json::parse((*Buffer)->getBuffer());
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    EXPECT_EQ(*Report, Result);
  }
}

TEST_F(ProcessPublic,
       CLIHasTheSameReportAndSeparatesGuestFailureFromIncomplete) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(Prefix, Directory));
  const std::filesystem::path Root(Directory.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Output = (Root / OutputFile).string();
  for (const char *Mode : {Normal, Loop, Fault, Unknown}) {
    SCOPED_TRACE(Mode);
    const auto Options = options(Mode);
    const auto Command = test::shellQuote(NEVERD_PROCESS_CLI) + " " +
                         process_cli::Command + " " + test::shellQuote(Path) +
                         " --" + process_cli::ProfileOption + "=" + LinuxELF64 +
                         " --" + process_cli::OptionsOption + "=" +
                         test::shellQuote(Options) +
                         test::redirectStdout(Output) + test::silenceStderr();
    EXPECT_EQ(test::systemExitCode(test::runShellCommand(Command)),
              Mode == Normal ? process_cli::GuestFailure
                             : process_cli::Incomplete);
    auto Buffer = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Buffer));
    auto Report = llvm::json::parse((*Buffer)->getBuffer());
    ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
    EXPECT_EQ(*Report, run(Options));
  }
}
} // namespace
