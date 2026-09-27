//===- DriverWDMPoFxTests.cpp - Genuine PoFx callback execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute genuine WDM component callbacks and blocking continuations.
///
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_POFX_FIXTURE
std::vector<const char *> images() {
  std::vector<const char *> Result{NEVERD_WDM_POFX_FIXTURE};
#ifdef NEVERD_WDM_POFX_CFG_FIXTURE
  Result.push_back(NEVERD_WDM_POFX_CFG_FIXTURE);
#endif
  return Result;
}
DriverRequest pnp(DevicePnpRequest Minor) {
  DriverRequest Request;
  Request.Kind = DriverRequestKind::Pnp;
  Request.DeviceID = "pofx";
  Request.Pnp = DriverPnpOperation{Minor, DriverBusCompletion{0, 0}};
  return Request;
}
DriverRequest file(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.DeviceID = "pofx";
  Request.File = 1;
  return Request;
}
DriverOptions options(char Mode) {
  DriverOptions Options;
  Options.Unload = true;
  DriverPnpDevice Device;
  Device.ID = "pofx";
  Device.InitialDevicePower = DevicePowerState::D0;
  Device.InitialSystemPower = SystemPowerState::Working;
  Device.InitialReportedDevicePower = DevicePowerState::D0;
  Options.PnpDevices.push_back(Device);
  auto Transfer = file(DriverRequestKind::DeviceControl);
  Transfer.ControlCode = 0x222000;
  Transfer.Input = {uint8_t(Mode)};
  if (Mode == 'F' || Mode == 'G')
    Transfer.PowerPolicyEvents.push_back(
        {1, "pofx", DriverPowerPolicyAction::ComponentIdleState, 0, 1});
  if (Mode == 'P')
    Transfer.PowerPolicyEvents.push_back(
        {1, "pofx", DriverPowerPolicyAction::PowerNotRequired});
  Options.Requests = {pnp(DevicePnpRequest::Start),
                      file(DriverRequestKind::Create),
                      Transfer,
                      file(DriverRequestKind::Cleanup),
                      file(DriverRequestKind::Close),
                      pnp(DevicePnpRequest::QueryRemove),
                      pnp(DevicePnpRequest::Remove)};
  return Options;
}
bool message(const DriverResult &Result, llvm::StringRef Text) {
  return std::any_of(Result.Messages.begin(), Result.Messages.end(),
                     [&](const auto &Message) {
                       return Message.find(Text.str()) != std::string::npos;
                     });
}
void clean(const DriverResult &Result) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.Devices.empty());
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.IOStatus, 0u);
  }
  EXPECT_FALSE(message(Result, "PoFx failure:"));
  EXPECT_TRUE(message(Result, "PoFx removed"));
  EXPECT_TRUE(message(Result, "PoFx unloaded"));
}
TEST(DriverWDMPoFx, BlockingAndAsyncCallbacksExecuteNormalCfgAndRebasedImages) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'A', 'B', 'D'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Mode);
        auto Options = options(Mode);
        Options.LoadAddress = Address;
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        EXPECT_TRUE(message(*Result, "PoFx active callback"));
        if (Mode == 'D')
          EXPECT_TRUE(message(*Result, "PoFx delayed idle acknowledged"));
        if (Mode != 'A')
          EXPECT_TRUE(message(*Result, "PoFx blocking cycle complete"));
      }
}
TEST(DriverWDMPoFx, ExplicitFxAndPowerDecisionsReturnThroughTheOriginalCaller) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'F', 'G', 'P'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Mode);
        auto Options = options(Mode);
        Options.LoadAddress = Address;
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result);
        ASSERT_EQ(Result->PowerPolicyEvents.size(), 1u);
        EXPECT_EQ(Result->PowerPolicyEvents[0].OccurredAt100ns, 1u);
        if (Mode == 'F' || Mode == 'G') {
          EXPECT_TRUE(message(*Result, "PoFx state F1"));
          EXPECT_TRUE(message(*Result, Mode == 'G'
                                           ? "PoFx delayed F0 acknowledged"
                                           : "PoFx state F0"));
        } else {
          EXPECT_TRUE(message(*Result, "PoFx power not required acknowledged"));
          EXPECT_TRUE(message(*Result, "PoFx power required acknowledged"));
        }
      }
}
#else
TEST(DriverWDMPoFx, OptionalGenuineFixture) {
  GTEST_SKIP() << "set NEVERD_WDM_POFX_FIXTURE to the genuine WDK PoFx fixture";
}
#endif
} // namespace
} // namespace neverd::emulation
