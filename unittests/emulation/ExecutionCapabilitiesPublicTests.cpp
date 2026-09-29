//===- ExecutionCapabilitiesPublicTests.cpp - CPU query C API and CLI ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionCLIStrings.h"
#include "neverd/emulation/ExecutionReportFields.h"
#include "neverd/sdk/NeverDCAPICPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>

namespace {
namespace field = neverd::emulation::execution_report;
#define NEVERD_CONFIGURATION_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_TEXT

std::string takeString(const char *Value) {
  if (!Value)
    return {};
  std::string Copy(Value);
  neverd_free_string(Value);
  return Copy;
}

TEST(ExecutionCapabilitiesPublic, QueryDoesNotRequireOrLoadAnImage) {
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  auto Text = takeString(neverd_cpu_capabilities_json(Session, nullptr, 0));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  auto Report = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  auto *Root = Report->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getInteger(field::SchemaVersion),
            NEVERD_CPU_CAPABILITIES_SCHEMA_VERSION);
  ASSERT_NE(Root->get(field::Host), nullptr);
  EXPECT_TRUE(Root->get(field::Host)->getAsNull());
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  Text = takeString(neverd_cpu_capabilities_json(Session, ARMConfiguration, 1));
  ASSERT_FALSE(Text.empty()) << takeString(neverd_last_error(Session));
  Report = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  auto *Host = Report->getAsObject()->getObject(field::Host);
  ASSERT_NE(Host, nullptr);
  EXPECT_TRUE(Host->getString(field::Availability));
  EXPECT_EQ(Host->getString(field::Scope), field::InitializationScope);
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
}

TEST(ExecutionCapabilitiesPublic,
     MalformedInputHasAnOwnedDiagnosticAndCanRecover) {
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  EXPECT_EQ(neverd_cpu_capabilities_json(nullptr, nullptr, 0), nullptr);
#define NEVERD_CONFIGURATION_JSON_INVALID(Name, Text)                          \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    EXPECT_EQ(neverd_cpu_capabilities_json(Session, Text, 0), nullptr);        \
    EXPECT_FALSE(takeString(neverd_last_error(Session)).empty());              \
  }
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_JSON_INVALID
  std::string Large(NEVERD_CPU_CONFIGURATION_JSON_LIMIT + 1, ' ');
  EXPECT_EQ(neverd_cpu_capabilities_json(Session, Large.c_str(), 0), nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)),
            field::ConfigurationTooLarge);
  EXPECT_EQ(neverd_cpu_capabilities_json(Session, nullptr, 2), nullptr);
  EXPECT_EQ(takeString(neverd_last_error(Session)), InvalidProbeError);
  EXPECT_FALSE(
      takeString(neverd_cpu_capabilities_json(Session, nullptr, 0)).empty());
  EXPECT_TRUE(takeString(neverd_last_error(Session)).empty());
}

TEST(ExecutionCapabilitiesPublic, CLIUsesTheSameConfigurationAndReport) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(TemporaryPrefix, Directory));
  const std::filesystem::path Path(Directory.str().str());
  auto Remove = llvm::scope_exit([&] { std::filesystem::remove_all(Path); });
  const auto Output = (Path / OutputFile).string();
  using namespace neverd;
  const auto Command = test::shellQuote(NEVERD_CPU_CLI) + " " +
                       execution_cli::CapabilitiesCommand + " --" +
                       execution_cli::ConfigurationOption + "=" +
                       test::shellQuote(ARMConfiguration) +
                       test::redirectStdout(Output) + test::silenceStderr();
  ASSERT_EQ(test::systemExitCode(test::runShellCommand(Command)), 0);
  auto Buffer = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Buffer));
  auto CLIReport = llvm::json::parse((*Buffer)->getBuffer());
  ASSERT_TRUE(bool(CLIReport)) << llvm::toString(CLIReport.takeError());
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  auto Text =
      takeString(neverd_cpu_capabilities_json(Session, ARMConfiguration, 0));
  auto APIReport = llvm::json::parse(Text);
  ASSERT_TRUE(bool(APIReport)) << llvm::toString(APIReport.takeError());
  EXPECT_EQ(*CLIReport, *APIReport);
}
} // namespace
