//===- DriverKMDFUsbIdlePublicTests.cpp - USB idle public execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "fixtures/driver_kmdf_usb_idle_test.h"
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

class DriverKMDFUsbIdlePublic : public ::testing::Test {
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

  void checkReport(llvm::StringRef Text, bool DirectRead = false) {
    const size_t Suspended = 5 + unsigned(DirectRead);
    const size_t Resumed = 7 + unsigned(DirectRead);
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
    const llvm::json::Object *Idle = nullptr, *D2 = nullptr, *D0 = nullptr;
    size_t ScenarioIndex = 0;
    unsigned Snapshots = 0;
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      if (Request->getString("origin") == "framework_usb_idle") {
        EXPECT_EQ(Idle, nullptr);
        Idle = Request;
        EXPECT_EQ(Request->getString("kind"), "internal_ioctl");
        EXPECT_EQ(Request->getInteger("code"), KmdfUsbInternalIdleIoctl);
        EXPECT_EQ(Request->getString("device_id"), "usb-function");
        ASSERT_TRUE(Request->get("file"));
        EXPECT_EQ(Request->get("file")->getAsNull(), nullptr);
        ASSERT_TRUE(Request->get("response_index"));
        EXPECT_EQ(Request->get("response_index")->getAsNull(), nullptr);
      }
      if (Request->getString("origin") == "framework_power_policy") {
        const auto *Power = Request->getObject("power");
        ASSERT_NE(Power, nullptr);
        if (Request->getInteger("response_index") == 0) {
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
      EXPECT_EQ(Request->getInteger("io_status"), 0);
      if (Index != Suspended && Index != Resumed)
        continue;
      std::string Bytes;
      ASSERT_TRUE(llvm::tryGetFromHex(
          Request->getString("output_hex").value_or(""), Bytes));
      ASSERT_EQ(Bytes.size(), KmdfUsbSnapshotWords * sizeof(uint32_t));
      const auto Word = [&](unsigned Offset) {
        uint32_t Value = 0;
        for (size_t I = 0; I < sizeof(Value); ++I)
          Value |= uint32_t(uint8_t(Bytes[Offset * sizeof(Value) + I]))
                   << (I * 8);
        return Value;
      };
      EXPECT_EQ(Word(KmdfUsbFailures), 0u);
      EXPECT_EQ(Word(KmdfUsbD0Entries), Index == Suspended ? 1u : 2u);
      EXPECT_EQ(Word(KmdfUsbD0Exits), 1u);
      EXPECT_EQ(Word(KmdfUsbInD0), Index == Suspended ? 0u : 1u);
      EXPECT_EQ(Word(KmdfUsbReadsDelivered), Index == Suspended ? 0u : 1u);
      EXPECT_EQ(Word(KmdfUsbArms), 0u);
      EXPECT_EQ(Word(KmdfUsbReadsRouted),
                Index == Suspended || DirectRead ? 0u : 1u);
      if (Index == Resumed) {
        if (DirectRead)
          EXPECT_EQ(Word(KmdfUsbReadRouteSequence), 0u);
        else
          EXPECT_LT(Word(KmdfUsbReadRouteSequence), Word(KmdfUsbEntrySequence));
        EXPECT_LT(Word(KmdfUsbEntrySequence),
                  Word(KmdfUsbReadDeliverySequence));
      }
      ++Snapshots;
    }
    ASSERT_NE(Idle, nullptr);
    ASSERT_NE(D2, nullptr);
    ASSERT_NE(D0, nullptr);
    EXPECT_EQ(Snapshots, 2u);
    EXPECT_EQ(Idle->getInteger("io_status"), uint32_t(KmdfUsbCancelledStatus));
    const auto *Usb = Idle->getObject("usb_idle");
    ASSERT_NE(Usb, nullptr);
    EXPECT_EQ(Usb->getString("completion_cause"), "cancel");
    EXPECT_EQ(Usb->getString("d2_irp"), D2->getString("irp"));
    EXPECT_EQ(Usb->getInteger("d2_status"), 0);
    EXPECT_EQ(Usb->getInteger("d2_completed_at_100ns"),
              D2->getObject("power")->getInteger("bus_completed_at_100ns"));
    ASSERT_TRUE(Usb->getInteger("completed_at_100ns"));
    ASSERT_TRUE(D0->getObject("power")->getInteger("bus_completed_at_100ns"));
    EXPECT_LT(*Usb->getInteger("completed_at_100ns"),
              *D0->getObject("power")->getInteger("bus_completed_at_100ns"));
    const auto *Devices =
        Report->getObject("configuration")->getArray("pnp_devices");
    ASSERT_NE(Devices, nullptr);
    const auto *Facts = Devices->front().getAsObject()->getObject("usb_idle");
    ASSERT_NE(Facts, nullptr);
    EXPECT_EQ(Facts->getString("device_wake"), "D2");
    EXPECT_EQ(Facts->getBoolean("remote_wake"), false);
    const auto *Events = Report->getArray("power_policy_events");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 2u);
    const auto *Permission = Events->back().getAsObject();
    EXPECT_EQ(Permission->getString("action"), "usb_idle_permission");
    const auto *Members = Permission->getArray("usb_idle_members");
    ASSERT_NE(Members, nullptr);
    ASSERT_EQ(Members->size(), 1u);
    EXPECT_EQ(Members->front().getAsObject()->getString("irp"),
              Idle->getString("irp"));
    EXPECT_EQ(Members->front().getAsObject()->getInteger("start_epoch"),
              Usb->getInteger("start_epoch"));
  }
};

TEST_F(DriverKMDFUsbIdlePublic,
       CAPIAndCLIKeepFrameworkIdleAndManagedD0Ordered) {
#ifdef NEVERD_KMDF_USB_IDLE_FIXTURE
  auto Parsed =
      llvm::json::parse(readFile(NEVERD_DRIVER_KMDF_USB_IDLE_SCENARIO));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Object = Parsed->getAsObject();
  ASSERT_NE(Object, nullptr);
  const std::vector<const char *> Images{
      NEVERD_KMDF_USB_IDLE_FIXTURE,
#ifdef NEVERD_KMDF_USB_IDLE_CFG_FIXTURE
      NEVERD_KMDF_USB_IDLE_CFG_FIXTURE,
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
  GTEST_SKIP() << "NEVERD_KMDF_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}
TEST_F(DriverKMDFUsbIdlePublic, CAPIAndCLIRouteReadDirectlyAfterManagedD0) {
#ifdef NEVERD_KMDF_USB_IDLE_FIXTURE
  auto Parsed =
      llvm::json::parse(readFile(NEVERD_DRIVER_KMDF_USB_IDLE_SCENARIO));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Object = Parsed->getAsObject();
  ASSERT_NE(Object, nullptr);
  auto *Requests = Object->getArray("requests");
  ASSERT_NE(Requests, nullptr);
  Requests->insert(
      Requests->begin() + 3,
      llvm::json::Object{{"kind", "ioctl"},
                         {"device_id", "usb-function"},
                         {"file", 1},
                         {"code", uint32_t(KmdfUsbDirectReadIoctl)}});
  const std::vector<const char *> Images{
      NEVERD_KMDF_USB_IDLE_FIXTURE,
#ifdef NEVERD_KMDF_USB_IDLE_CFG_FIXTURE
      NEVERD_KMDF_USB_IDLE_CFG_FIXTURE,
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
      checkReport(API, true);
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
      checkReport(readFile(Output), true);
    }
#else
  GTEST_SKIP() << "NEVERD_KMDF_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
