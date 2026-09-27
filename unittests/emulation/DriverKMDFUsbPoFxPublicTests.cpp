//===- DriverKMDFUsbPoFxPublicTests.cpp - USB idle public execution -------===//
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

class DriverKMDFUsbPoFxPublic : public ::testing::Test {
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
    const llvm::json::Object *Idle = nullptr, *D2 = nullptr, *D0 = nullptr;
    std::vector<std::vector<uint32_t>> Snapshots;
    for (const auto &Value : *Requests) {
      const auto *Request = Value.getAsObject();
      ASSERT_NE(Request, nullptr);
      EXPECT_EQ(Request->getBoolean("completed"), true);
      if (Request->getString("origin") == "framework_usb_idle") {
        EXPECT_EQ(Idle, nullptr);
        Idle = Request;
        EXPECT_EQ(Request->getString("kind"), "internal_ioctl");
        EXPECT_EQ(Request->getInteger("code"), KmdfUsbInternalIdleIoctl);
      } else if (Request->getString("origin") == "framework_power_policy") {
        if (Request->getInteger("response_index") == 0) {
          EXPECT_EQ(D2, nullptr);
          D2 = Request;
        } else {
          EXPECT_EQ(D0, nullptr);
          EXPECT_EQ(Request->getInteger("response_index"), 1);
          D0 = Request;
        }
      } else if (Request->getString("origin") == "scenario") {
        EXPECT_EQ(Request->getInteger("io_status"), 0);
        if (Request->getString("kind") == "read")
          EXPECT_EQ(Request->getString("output_hex"), "49534255");
        if (Request->getInteger("code") != KmdfUsbPoFxSnapshotIoctl)
          continue;
        std::string Bytes;
        ASSERT_TRUE(llvm::tryGetFromHex(
            Request->getString("output_hex").value_or(""), Bytes));
        ASSERT_EQ(Bytes.size(), KmdfUsbPoFxSnapshotWords * sizeof(uint32_t));
        std::vector<uint32_t> Words(KmdfUsbPoFxSnapshotWords);
        for (size_t I = 0; I < Words.size(); ++I)
          for (size_t B = 0; B < sizeof(uint32_t); ++B)
            Words[I] |= uint32_t(uint8_t(Bytes[I * sizeof(uint32_t) + B]))
                        << (B * 8);
        EXPECT_EQ(Words[KmdfUsbFailures], 0u);
        EXPECT_EQ(Words[KmdfUsbReadsRouted], 0u);
        Snapshots.push_back(std::move(Words));
      }
    }
    ASSERT_EQ(Snapshots.size(), 5u);
    const auto &Parked = Snapshots[1], &Suspended = Snapshots[3],
               &Resumed = Snapshots[4];
    EXPECT_EQ(Parked[KmdfUsbInD0], 1u);
    EXPECT_EQ(Parked[KmdfUsbD0Exits], 0u);
    EXPECT_EQ(Parked[KmdfUsbPoFxActive], 0u);
    EXPECT_EQ(Parked[KmdfUsbPoFxCurrentState], 1u);
    EXPECT_EQ(Suspended[KmdfUsbInD0], 0u);
    EXPECT_EQ(Suspended[KmdfUsbTargetState], 3u);
    EXPECT_EQ(Suspended[KmdfUsbReadsDelivered], 0u);
    EXPECT_EQ(Resumed[KmdfUsbInD0], 1u);
    EXPECT_EQ(Resumed[KmdfUsbD0Entries], 2u);
    EXPECT_EQ(Resumed[KmdfUsbPoFxPosts], 1u);
    EXPECT_EQ(Resumed[KmdfUsbPoFxActive], 1u);
    EXPECT_EQ(Resumed[KmdfUsbPoFxF0Transitions], 1u);
    EXPECT_EQ(Resumed[KmdfUsbPoFxF1Transitions], 1u);
    EXPECT_EQ(Resumed[KmdfUsbReadsDelivered], 1u);
    EXPECT_LT(Resumed[KmdfUsbEntrySequence], Resumed[KmdfUsbPoFxF0Sequence]);
    EXPECT_LT(Resumed[KmdfUsbPoFxF0Sequence],
              Resumed[KmdfUsbPoFxActiveSequence]);
    EXPECT_LT(Resumed[KmdfUsbPoFxActiveSequence],
              Resumed[KmdfUsbReadDeliverySequence]);
    ASSERT_NE(Idle, nullptr);
    ASSERT_NE(D2, nullptr);
    ASSERT_NE(D0, nullptr);
    const auto *Usb = Idle->getObject("usb_idle");
    ASSERT_NE(Usb, nullptr);
    EXPECT_EQ(Usb->getString("completion_cause"), "cancel");
    EXPECT_EQ(Usb->getString("d2_irp"), D2->getString("irp"));
    EXPECT_EQ(Usb->getInteger("d2_status"), 0);
    const auto *Down = D2->getObject("power"), *Up = D0->getObject("power");
    ASSERT_NE(Down, nullptr);
    ASSERT_NE(Up, nullptr);
    EXPECT_EQ(Down->getString("device_state_after"), "D2");
    EXPECT_EQ(Up->getString("device_state_after"), "D0");
    EXPECT_EQ(Usb->getInteger("d2_completed_at_100ns"),
              Down->getInteger("bus_completed_at_100ns"));
    ASSERT_TRUE(Usb->getInteger("callback_returned_at_100ns"));
    ASSERT_TRUE(Down->getInteger("bus_completed_at_100ns"));
    EXPECT_LE(*Down->getInteger("bus_completed_at_100ns"),
              *Usb->getInteger("callback_returned_at_100ns"));
    ASSERT_TRUE(Usb->getInteger("completed_at_100ns"));
    ASSERT_TRUE(Up->getInteger("bus_completed_at_100ns"));
    EXPECT_LT(*Usb->getInteger("completed_at_100ns"),
              *Up->getInteger("bus_completed_at_100ns"));
    const auto *Events = Report->getArray("power_policy_events");
    ASSERT_NE(Events, nullptr);
    ASSERT_EQ(Events->size(), 4u);
    EXPECT_EQ((*Events)[2].getAsObject()->getString("action"),
              "power_not_required");
    const auto *Members =
        Events->back().getAsObject()->getArray("usb_idle_members");
    ASSERT_NE(Members, nullptr);
    ASSERT_EQ(Members->size(), 1u);
    EXPECT_EQ(Members->front().getAsObject()->getString("irp"),
              Idle->getString("irp"));
  }
};

TEST_F(DriverKMDFUsbPoFxPublic,
       CAPIAndCLIRequireBothGrantsAndBothResumeAcknowledgements) {
#ifdef NEVERD_KMDF_USB_IDLE_FIXTURE
  auto Parsed =
      llvm::json::parse(readFile(NEVERD_DRIVER_KMDF_USB_POFX_SCENARIO));
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
    for (const char *Service : {"NeverDKmdfUsbIdleS", "NeverDKmdfUsbIdleH"})
      for (const char *Base : {"0x0", "0x190000000"}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Base);
        (*Object)["load_address"] = Base;
        (*Object)["service_name"] = Service;
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
        EXPECT_EQ(neverd::test::systemExitCode(
                      neverd::test::runShellCommand(Command)),
                  0)
            << readFile(Error);
        checkReport(readFile(Output));
      }
#else
  GTEST_SKIP() << "NEVERD_KMDF_USB_IDLE_FIXTURE requires a genuine WDK fixture";
#endif
}
} // namespace
