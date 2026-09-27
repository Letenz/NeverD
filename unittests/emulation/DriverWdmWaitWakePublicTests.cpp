//===- DriverWdmWaitWakePublicTests.cpp - Native wake public execution --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "fixtures/driver_wdm_wait_wake_test.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIEmulation.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
std::string takeString(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream Stream(Path);
  return {std::istreambuf_iterator<char>(Stream), {}};
}

class DriverWdmWaitWakePublic : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;

  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-wdm-wake-public",
                                                      Temporary));
    Directory = Temporary.str().str();
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }

  void checkReport(llvm::StringRef Text) {
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 22u);
    size_t ScenarioIndex = 0;
    unsigned Wakes = 0, Cancelled = 0, D0Children = 0, CheckedSnapshots = 0;
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      const auto *Power = Request->getObject("power");
      if (Request->getString("origin") == "PoRequestPowerIrp") {
        ASSERT_TRUE(Power);
        ASSERT_TRUE(Request->get("file"));
        EXPECT_EQ(Request->get("file")->getAsNull(), nullptr);
        ASSERT_TRUE(Power->getInteger("bus_received_at_100ns"));
        ASSERT_TRUE(Power->getInteger("bus_completed_at_100ns"));
        if (Power->getString("minor") == "wait_wake") {
          ++Wakes;
          ASSERT_TRUE(Request->get("response_index"));
          EXPECT_EQ(Request->get("response_index")->getAsNull(), nullptr);
          EXPECT_EQ(Power->getString("power_type"), "system");
          EXPECT_EQ(Power->getString("power_state"), "working");
          if (Request->getInteger("io_status") == WdmWakeCancelledStatus) {
            ++Cancelled;
            EXPECT_TRUE(Request->getInteger("cancel_requested_at_100ns"));
            ASSERT_TRUE(Power->get("wake_source_device_id"));
            EXPECT_EQ(Power->get("wake_source_device_id")->getAsNull(),
                      nullptr);
          } else {
            EXPECT_EQ(Request->getInteger("io_status"), 0);
            EXPECT_EQ(Power->getString("wake_source_device_id"), "native-wake");
          }
          continue;
        }
        EXPECT_EQ(Request->getInteger("response_index"), D0Children++);
        EXPECT_EQ(Power->getString("minor"), "set");
        EXPECT_EQ(Power->getString("power_type"), "device");
        EXPECT_EQ(Power->getString("power_state"), "D0");
      }
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      if (Request->getString("origin") != "scenario")
        continue;
      const size_t Index = ScenarioIndex++;
      if (Index != 2 && Index != 6 && Index != 9 && Index != 12)
        continue;
      EXPECT_EQ(Request->getInteger("code"), WdmWakeSnapshotIoctl);
      std::string Bytes;
      ASSERT_TRUE(llvm::tryGetFromHex(
          Request->getString("output_hex").value_or(""), Bytes));
      ASSERT_EQ(Bytes.size(), WdmWakeSnapshotWords * sizeof(uint32_t));
      const auto Word = [&](unsigned Offset) {
        uint32_t Value = 0;
        for (size_t I = 0; I < sizeof(Value); ++I)
          Value |= uint32_t(uint8_t(Bytes[Offset * sizeof(Value) + I]))
                   << (I * 8);
        return Value;
      };
      const unsigned Sends = Index <= 6 ? 1 : Index == 9 ? 2 : 3;
      const unsigned Completions = Index == 2 ? 0 : Sends;
      EXPECT_EQ(Word(WdmWakeFailures), 0u);
      EXPECT_EQ(Word(WdmWakeSubmissions), Sends);
      EXPECT_EQ(Word(WdmWakeIoCompletions), Completions);
      EXPECT_EQ(Word(WdmWakeCallbacks), Completions);
      EXPECT_EQ(Word(WdmWakeActive), unsigned(Index == 2));
      EXPECT_EQ(Word(WdmWakeHasCancelRoutine), unsigned(Index == 2));
      EXPECT_EQ(Word(WdmWakePublishedOutputs), Sends);
      EXPECT_EQ(Word(WdmWakeD0Callbacks), Index == 2   ? 0u
                                          : Index == 6 ? 1u
                                                       : 2u);
      EXPECT_EQ(Word(WdmWakeWorkers), unsigned(Index >= 9));
      EXPECT_EQ(Word(WdmWakeCancelTrue), unsigned(Index >= 9));
      EXPECT_EQ(Word(WdmWakeLastIoIrql), Index == 9 ? 2u : 0u);
      EXPECT_EQ(Word(WdmWakeLastCallbackIrql), Index == 9 ? 2u : 0u);
      ++CheckedSnapshots;
    }
    EXPECT_EQ(Wakes, 3u);
    EXPECT_EQ(Cancelled, 1u);
    EXPECT_EQ(D0Children, 2u);
    EXPECT_EQ(CheckedSnapshots, 4u);
  }

  void checkElevatedReport(llvm::StringRef Text) {
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    const auto *Report = Parsed->getAsObject();
    ASSERT_NE(Report, nullptr);
    EXPECT_EQ(Report->getString("stop_reason"), "returned")
        << Report->getString("diagnostic").value_or("").str();
    EXPECT_EQ(Report->getBoolean("scenario_success"), true);
    EXPECT_EQ(Report->getBoolean("unload_completed"), true);
    const auto *Requests = Report->getArray("requests");
    ASSERT_NE(Requests, nullptr);
    ASSERT_EQ(Requests->size(), 17u);
    unsigned Wakes = 0, Children = 0, Snapshots = 0;
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      if (Request->getString("origin") == "PoRequestPowerIrp") {
        const auto *Power = Request->getObject("power");
        ASSERT_NE(Power, nullptr);
        ASSERT_TRUE(Request->get("file"));
        EXPECT_EQ(Request->get("file")->getAsNull(), nullptr);
        if (Power->getString("minor") == "wait_wake") {
          ++Wakes;
          EXPECT_EQ(Request->getInteger("io_status"), WdmWakeCancelledStatus);
          EXPECT_TRUE(Request->getInteger("cancel_requested_at_100ns"));
          EXPECT_EQ(Power->getString("device_state_after"), "D2");
          continue;
        }
        EXPECT_EQ(Request->getInteger("response_index"), Children);
        EXPECT_EQ(Power->getString("minor"), Children == 1 ? "query" : "set");
        EXPECT_EQ(Power->getString("power_state"), Children ? "D2" : "D0");
        EXPECT_EQ(Power->getString("device_state_before"),
                  Children ? "D0" : "D2");
        EXPECT_EQ(Power->getString("device_state_after"),
                  Children == 2 ? "D2" : "D0");
        ASSERT_TRUE(Power->getInteger("bus_received_at_100ns"));
        ASSERT_TRUE(Power->getInteger("bus_completed_at_100ns"));
        ++Children;
      }
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      if (Request->getInteger("code") != WdmWakeExtendedSnapshotIoctl)
        continue;
      std::string Bytes;
      ASSERT_TRUE(llvm::tryGetFromHex(
          Request->getString("output_hex").value_or(""), Bytes));
      ASSERT_EQ(Bytes.size(), WdmWakeExtendedSnapshotWords * sizeof(uint32_t));
      const auto Word = [&](unsigned Offset) {
        uint32_t Value = 0;
        for (size_t I = 0; I < sizeof(Value); ++I)
          Value |= uint32_t(uint8_t(Bytes[Offset * sizeof(Value) + I]))
                   << (I * 8);
        return Value;
      };
      ++Snapshots;
      EXPECT_EQ(Word(WdmWakeFailures), 0u);
      EXPECT_EQ(Word(WdmWakeQueuedSubmissions), Snapshots);
      EXPECT_EQ(Word(WdmWakeQueuedCompletions), Snapshots);
      EXPECT_EQ(Word(WdmWakeQueuedDispatches), Snapshots);
      EXPECT_EQ(Word(WdmWakeD0Callbacks), 1u);
      EXPECT_EQ(Word(WdmWakeWorkers), 0u);
      EXPECT_EQ(Word(WdmWakeCancelTrue), 1u);
      EXPECT_EQ(Word(WdmWakeActive), 0u);
      EXPECT_EQ(Word(WdmWakeLastIoIrql), 2u);
      EXPECT_EQ(Word(WdmWakeLastCallbackIrql), 2u);
      EXPECT_EQ(Word(WdmWakeCallerIrqlBefore), 2u);
      EXPECT_EQ(Word(WdmWakeCallerIrqlAfter), 2u);
      EXPECT_EQ(Word(WdmWakeCallerCR8Before), 2u);
      EXPECT_EQ(Word(WdmWakeCallerCR8After), 2u);
      EXPECT_EQ(Word(WdmWakePowerDispatchIrql), 0u);
      EXPECT_EQ(Word(WdmWakePowerDispatchCR8), 0u);
      EXPECT_EQ(Word(WdmWakePowerCallbackIrql), 0u);
      EXPECT_EQ(Word(WdmWakePowerCallbackCR8), 0u);
      EXPECT_NE(Word(WdmWakeCallerReturnOrder), 0u);
      EXPECT_LT(Word(WdmWakeCallerReturnOrder),
                Word(WdmWakePowerDispatchOrder));
      EXPECT_LT(Word(WdmWakePowerDispatchOrder),
                Word(WdmWakePowerCallbackOrder));
    }
    EXPECT_EQ(Wakes, 1u);
    EXPECT_EQ(Children, 3u);
    EXPECT_EQ(Snapshots, 3u);
  }
};

TEST_F(DriverWdmWaitWakePublic, CAPIAndCLIExecuteIndependentWakeCancelAndD0) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  const auto Scenario = readFile(NEVERD_DRIVER_WDM_WAIT_WAKE_SCENARIO);
  ASSERT_FALSE(Scenario.empty());
  const std::vector<const char *> Images{
      NEVERD_WDM_WAIT_WAKE_FIXTURE,
#ifdef NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE
      NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE,
#endif
  };
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    const auto API = takeString(neverd_emulate_driver_scenario_json(
        Session, Image, Scenario.c_str(), nullptr));
    ASSERT_FALSE(API.empty()) << takeString(neverd_last_error(Session));
    checkReport(API);
    const auto Output = Directory / "report.json";
    const auto Error = Directory / "error.txt";
    const auto Command =
        neverd::test::shellQuote(NEVERD_DRIVER_CLI) + " emulate-driver " +
        neverd::test::shellQuote(Image) + " --scenario " +
        neverd::test::shellQuote(NEVERD_DRIVER_WDM_WAIT_WAKE_SCENARIO) +
        neverd::test::redirectOutput(Output.string(), Error.string());
    EXPECT_EQ(
        neverd::test::systemExitCode(neverd::test::runShellCommand(Command)), 0)
        << readFile(Error);
    checkReport(readFile(Output));
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST_F(DriverWdmWaitWakePublic,
       CAPIAndCLIPreserveElevatedCallersAndDispatchOrder) {
#ifdef NEVERD_WDM_WAIT_WAKE_FIXTURE
  auto Parsed =
      llvm::json::parse(readFile(NEVERD_DRIVER_WDM_ELEVATED_POWER_SCENARIO));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Object = Parsed->getAsObject();
  ASSERT_NE(Object, nullptr);
  const std::vector<const char *> Images{
      NEVERD_WDM_WAIT_WAKE_FIXTURE,
#ifdef NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE
      NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE,
#endif
  };
  for (const char *Image : Images)
    for (const char *Base : {"0x0", "0x190000000"}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      (*Object)["load_address"] = Base;
      std::string Scenario;
      llvm::raw_string_ostream(Scenario) << *Parsed;
      const auto API = takeString(neverd_emulate_driver_scenario_json(
          Session, Image, Scenario.c_str(), nullptr));
      ASSERT_FALSE(API.empty()) << takeString(neverd_last_error(Session));
      checkElevatedReport(API);
      const auto Input = Directory / "elevated-power.json";
      std::ofstream(Input) << Scenario;
      const auto Output = Directory / "report.json";
      const auto Error = Directory / "error.txt";
      const auto Command =
          neverd::test::shellQuote(NEVERD_DRIVER_CLI) + " emulate-driver " +
          neverd::test::shellQuote(Image) + " --scenario " +
          neverd::test::shellQuote(Input.string()) +
          neverd::test::redirectOutput(Output.string(), Error.string());
      EXPECT_EQ(
          neverd::test::systemExitCode(neverd::test::runShellCommand(Command)),
          0)
          << readFile(Error);
      checkElevatedReport(readFile(Output));
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_WAIT_WAKE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
