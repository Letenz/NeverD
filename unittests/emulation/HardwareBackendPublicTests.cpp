//===- HardwareBackendPublicTests.cpp - Public selection tests-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/DriverReportFields.h"
#include "neverd/emulation/ExecutionBackend.h"
#include "neverd/emulation/ExecutionReportFields.h"
#include "neverd/sdk/NeverDCAPICPU.h"
#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/JSON.h"

#include <filesystem>

namespace {
#define NEVERD_HARDWARE_DRIVER_CASE(Name, Text) constexpr char Name[] = Text;
#include "HardwareBackendCases.def"
#undef NEVERD_HARDWARE_DRIVER_CASE
#define NEVERD_CONFIGURATION_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_TEXT
namespace availability {
#define NEVERD_EXECUTION_AVAILABILITY(Name, Text)                              \
  inline constexpr char Name[] = Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_AVAILABILITY
} // namespace availability
TEST(HardwareBackendPublic, ValidatesSelectionAndPreservesV1Contract) {
  using namespace neverd::emulation;
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  const auto Path =
      (std::filesystem::path(NEVERD_DRIVER_FIXTURES) / Success).string();
  EXPECT_EQ(neverd_emulate_driver_backend_json(Session, Path.c_str(), nullptr,
                                               nullptr, nullptr,
                                               execution::Legacy),
            nullptr);
  EXPECT_EQ(neverd_emulate_driver_backend_json(Session, Path.c_str(), nullptr,
                                               nullptr, execution::KVM,
                                               execution::Software),
            nullptr);
  const char *Text = neverd_emulate_driver_backend_json(
      Session, Path.c_str(), nullptr, nullptr, execution::Unicorn,
      execution::Legacy);
  ASSERT_NE(Text, nullptr);
  auto Report = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  ASSERT_NE(Report->getAsObject(), nullptr);
  EXPECT_EQ(Report->getAsObject()->getString(execution::SelectedBackend),
            execution::Unicorn);
  Text = neverd_emulate_driver_json(Session, Path.c_str(), nullptr);
  ASSERT_NE(Text, nullptr);
  Report = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  ASSERT_NE(Report->getAsObject(), nullptr);
  EXPECT_FALSE(Report->getAsObject()->get(execution::SelectedBackend));
}

TEST(HardwareBackendPublic, AutoDriverUsesReportedHostSelection) {
  using namespace neverd::emulation;
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  const auto Path =
      (std::filesystem::path(NEVERD_DRIVER_FIXTURES) / Success).string();
  const char *Query =
      neverd_cpu_capabilities_json(Session, DriverAutoConfiguration, 1);
  ASSERT_NE(Query, nullptr);
  auto Capabilities = llvm::json::parse(Query);
  neverd_free_string(Query);
  ASSERT_TRUE(bool(Capabilities)) << llvm::toString(Capabilities.takeError());
  ASSERT_NE(Capabilities->getAsObject(), nullptr);
  namespace cpu = execution_report;
  const auto *Host = Capabilities->getAsObject()->getObject(cpu::Host);
  ASSERT_NE(Host, nullptr);
  ASSERT_TRUE(Host->getString(cpu::Availability).has_value());
  if (Host->getString(cpu::Availability) != availability::Available)
    GTEST_SKIP() << Host->getString(cpu::Availability)->str();
  const auto *Configuration =
      Capabilities->getAsObject()->getObject(cpu::Configuration);
  ASSERT_NE(Configuration, nullptr);
  const char *Text = neverd_emulate_driver_backend_json(
      Session, Path.c_str(), nullptr, nullptr, execution::Auto,
      execution::Legacy);
  ASSERT_NE(Text, nullptr);
  auto Report = llvm::json::parse(Text);
  neverd_free_string(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  auto *Root = Report->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString(execution::RequestedBackend), execution::Auto);
  EXPECT_EQ(Root->getString(execution::SelectedBackend),
            Configuration->getString(cpu::Backend));
  EXPECT_EQ(Root->getString(execution::ExecutionContract), execution::Legacy);
  EXPECT_EQ(Root->getString(field::StopReason), stop::Returned);
}
} // namespace
