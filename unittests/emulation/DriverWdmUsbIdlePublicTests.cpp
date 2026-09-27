//===- DriverWdmUsbIdlePublicTests.cpp - USB idle public execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "fixtures/driver_wdm_usb_idle_test.h"
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

class DriverWdmUsbIdlePublic : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;

  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-usb-idle-public",
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
    ASSERT_EQ(Requests->size(), 15u);
    const llvm::json::Object *Idle = nullptr, *D2 = nullptr, *D0 = nullptr,
                             *Wake = nullptr;
    size_t ScenarioIndex = 0;
    unsigned Snapshots = 0;
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      if (Request->getString("origin") == "driver_allocated_irp") {
        EXPECT_EQ(Idle, nullptr);
        Idle = Request;
        EXPECT_EQ(Request->getString("kind"), "internal_ioctl");
        EXPECT_EQ(Request->getInteger("code"), UsbIdleInternalIoctl);
        EXPECT_EQ(Request->getString("device_id"), "usb-function");
        ASSERT_TRUE(Request->get("file"));
        EXPECT_EQ(Request->get("file")->getAsNull(), nullptr);
      }
      if (Request->getString("origin") == "PoRequestPowerIrp") {
        const auto *Power = Request->getObject("power");
        ASSERT_NE(Power, nullptr);
        ASSERT_TRUE(Request->get("file"));
        EXPECT_EQ(Request->get("file")->getAsNull(), nullptr);
        if (Power->getString("minor") == "wait_wake") {
          Wake = Request;
          EXPECT_EQ(Power->getString("device_state_after"), "D2");
          EXPECT_EQ(Power->getString("wake_source_device_id"), "usb-function");
        } else if (Request->getInteger("response_index") == 0) {
          D2 = Request;
          EXPECT_EQ(Power->getString("device_state_before"), "D0");
          EXPECT_EQ(Power->getString("device_state_after"), "D2");
        } else {
          D0 = Request;
          EXPECT_EQ(Request->getInteger("response_index"), 1);
          EXPECT_EQ(Power->getString("device_state_before"), "D2");
          EXPECT_EQ(Power->getString("device_state_after"), "D0");
        }
      }
      if (Request->getString("origin") != "scenario")
        continue;
      const size_t Index = ScenarioIndex++;
      if (Index != 4 && Index != 6)
        continue;
      std::string Bytes;
      ASSERT_TRUE(llvm::tryGetFromHex(
          Request->getString("output_hex").value_or(""), Bytes));
      ASSERT_EQ(Bytes.size(), UsbIdleSnapshotWords * sizeof(uint32_t));
      const auto Word = [&](unsigned Offset) {
        uint32_t Value = 0;
        for (size_t I = 0; I < sizeof(Value); ++I)
          Value |= uint32_t(uint8_t(Bytes[Offset * sizeof(Value) + I]))
                   << (I * 8);
        return Value;
      };
      EXPECT_EQ(Word(UsbIdleFailures), 0u);
      EXPECT_EQ(Word(UsbIdleAllocations), 1u);
      EXPECT_EQ(Word(UsbIdleCallbackEntries), 1u);
      EXPECT_EQ(Word(UsbIdleCallbackReturns), 1u);
      EXPECT_EQ(Word(UsbIdleD2Completions), 1u);
      EXPECT_EQ(Word(UsbIdleD0Completions), unsigned(Index == 6));
      EXPECT_EQ(Word(UsbIdleFrees), unsigned(Index == 6));
      EXPECT_EQ(Word(UsbIdleCompletions), unsigned(Index == 6));
      EXPECT_EQ(Word(UsbIdleActive), unsigned(Index == 4));
      EXPECT_EQ(Word(UsbIdleWakeActive), unsigned(Index == 4));
      EXPECT_EQ(Word(UsbIdleWakeCallbacks), unsigned(Index == 6));
      EXPECT_EQ(Word(UsbIdleCompletedBeforeD0Acknowledgement),
                unsigned(Index == 6));
      EXPECT_LT(Word(UsbIdleEntryOrder), Word(UsbIdleD2CompletionOrder));
      EXPECT_LT(Word(UsbIdleD2CompletionOrder), Word(UsbIdleReturnOrder));
      if (Index == 6) {
        EXPECT_LT(Word(UsbIdleReturnOrder), Word(UsbIdleD0DispatchOrder));
        EXPECT_LT(Word(UsbIdleD0DispatchOrder), Word(UsbIdleCompletionOrder));
        EXPECT_LT(Word(UsbIdleCompletionOrder), Word(UsbIdleD0CompletionOrder));
      }
      ++Snapshots;
    }
    ASSERT_NE(Idle, nullptr);
    ASSERT_NE(D2, nullptr);
    ASSERT_NE(D0, nullptr);
    ASSERT_NE(Wake, nullptr);
    EXPECT_EQ(Snapshots, 2u);
    const auto *Usb = Idle->getObject("usb_idle");
    ASSERT_NE(Usb, nullptr);
    EXPECT_EQ(Usb->getString("completion_cause"), "device_d0");
    EXPECT_EQ(Usb->getString("d2_irp"), D2->getString("irp"));
    EXPECT_EQ(Usb->getInteger("d2_status"), 0);
    EXPECT_EQ(Usb->getInteger("d2_completed_at_100ns"),
              D2->getObject("power")->getInteger("bus_completed_at_100ns"));
    EXPECT_EQ(Usb->getInteger("completed_at_100ns"),
              D0->getObject("power")->getInteger("bus_received_at_100ns"));
    ASSERT_TRUE(Usb->getInteger("completed_at_100ns"));
    ASSERT_TRUE(D0->getObject("power")->getInteger("bus_completed_at_100ns"));
    EXPECT_LT(*Usb->getInteger("completed_at_100ns"),
              *D0->getObject("power")->getInteger("bus_completed_at_100ns"));
    const auto *Events = Report->getArray("power_policy_events");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 2u);
    const auto *Permission = Events->front().getAsObject();
    ASSERT_NE(Permission, nullptr);
    EXPECT_EQ(Permission->getString("action"), "usb_idle_permission");
    EXPECT_TRUE(Permission->getInteger("occurred_at_100ns"));
    const auto *Members = Permission->getArray("usb_idle_members");
    ASSERT_NE(Members, nullptr);
    ASSERT_EQ(Members->size(), 1u);
    const auto *Member = Members->front().getAsObject();
    ASSERT_NE(Member, nullptr);
    EXPECT_EQ(Member->getString("device_id"), "usb-function");
    EXPECT_EQ(Member->getString("irp"), Idle->getString("irp"));
    EXPECT_EQ(Member->getInteger("start_epoch"),
              Usb->getInteger("start_epoch"));
  }
};

TEST_F(DriverWdmUsbIdlePublic, CAPIAndCLIKeepIdleD2WakeAndD0Independent) {
#ifdef NEVERD_WDM_USB_IDLE_FIXTURE
  auto Parsed =
      llvm::json::parse(readFile(NEVERD_DRIVER_WDM_USB_IDLE_SCENARIO));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Object = Parsed->getAsObject();
  ASSERT_NE(Object, nullptr);
  const std::vector<const char *> Images{
      NEVERD_WDM_USB_IDLE_FIXTURE,
#ifdef NEVERD_WDM_USB_IDLE_CFG_FIXTURE
      NEVERD_WDM_USB_IDLE_CFG_FIXTURE,
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
      checkReport(API);
      const auto Input = Directory / "usb-idle.json";
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
      checkReport(readFile(Output));
    }
#else
  GTEST_SKIP() << "NEVERD_WDM_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
