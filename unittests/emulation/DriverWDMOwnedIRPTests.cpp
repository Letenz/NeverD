//===- DriverWDMOwnedIRPTests.cpp - Genuine caller-owned internal IRPs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prove caller-owned packet lifetime independently of the outer file request,
/// including free in completion, retained MPR, worker and cancellation paths.
///
//===----------------------------------------------------------------------===//
#include "fixtures/driver_wdm_owned_irp_test.h"
#include "gtest/gtest.h"
#include "os/windows/kernel/KernelFramework.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
constexpr llvm::StringLiteral DeviceName = "\\Device\\NeverDOwnedIrp";

std::vector<const char *> images() {
  return {
      NEVERD_WDM_OWNED_IRP_FIXTURE,
#ifdef NEVERD_WDM_OWNED_IRP_CFG_FIXTURE
      NEVERD_WDM_OWNED_IRP_CFG_FIXTURE,
#endif
  };
}

DriverRequest file(DriverRequestKind Kind) {
  DriverRequest Request;
  Request.Kind = Kind;
  Request.File = 1;
  if (Kind == DriverRequestKind::Create)
    Request.Device = DeviceName.str();
  return Request;
}

DriverRequest control(uint32_t Code) {
  auto Request = file(DriverRequestKind::DeviceControl);
  Request.ControlCode = Code;
  if (Code == OwnedIrpSnapshotIoctl)
    Request.OutputSize = OwnedIrpSnapshotWords * sizeof(uint32_t);
  return Request;
}

DriverOptions options(uint8_t Mode, bool CallerStack) {
  DriverOptions Options;
  Options.ServiceName = "NeverDOwnedIrp";
  Options.Unload = true;
  auto Submit = control(OwnedIrpSubmitIoctl);
  Submit.Input = {Mode, uint8_t(CallerStack)};
  Options.Requests = {file(DriverRequestKind::Create), std::move(Submit),
                      control(OwnedIrpSnapshotIoctl)};
  if (Mode == OwnedIrpHold) {
    Options.Requests.push_back(control(OwnedIrpReleaseIoctl));
    Options.Requests.push_back(control(OwnedIrpSnapshotIoctl));
  }
  Options.Requests.push_back(file(DriverRequestKind::Cleanup));
  Options.Requests.push_back(file(DriverRequestKind::Close));
  return Options;
}

const DriverRequestResult *scenarioRequest(const DriverResult &Result,
                                           size_t Expected) {
  size_t Index = 0;
  for (const auto &Request : Result.Requests)
    if (Request.Origin == DriverRequestOrigin::Scenario && Index++ == Expected)
      return &Request;
  return nullptr;
}

uint32_t word(const DriverRequestResult &Request, size_t Index) {
  uint32_t Value = 0;
  if (Request.Output.size() >= (Index + 1) * sizeof(Value))
    for (size_t I = 0; I < sizeof(Value); ++I)
      Value |= uint32_t(Request.Output[Index * sizeof(Value) + I]) << (I * 8);
  return Value;
}

void exercise(uint8_t Mode) {
  for (const auto *Image : images())
    for (bool CallerStack : {false, true})
      for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(char(Mode));
        SCOPED_TRACE(CallerStack);
        SCOPED_TRACE(Base);
        auto Input = options(Mode, CallerStack);
        Input.LoadAddress = Base;
        auto Result = emulateDriver(Image, Input);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_TRUE(Result->Devices.empty());
        for (const auto &Message : Result->Messages)
          EXPECT_FALSE(
              llvm::StringRef(Message).starts_with("WDM owned IRP: invalid"))
              << Message;
        EXPECT_NE(
            std::find(
                Result->Messages.begin(), Result->Messages.end(),
                "WDM owned IRP: unload failures 0 allocations 1 frees 1\n"),
            Result->Messages.end());
        const bool Submitted = Mode != OwnedIrpUnsent;
        const bool Pending = Mode == OwnedIrpWorker || Mode == OwnedIrpCancel;
        const bool Nested = Pending || Mode == OwnedIrpNested;
        const uint32_t ChildStatus = Mode == OwnedIrpCancel
                                         ? framework::RequestCancelled
                                         : windows::StatusSuccess;
        ASSERT_EQ(Result->Requests.size(), Input.Requests.size() + Submitted);
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          if (Request.Origin == DriverRequestOrigin::Scenario) {
            EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
            EXPECT_EQ(Request.File, 1u);
          } else {
            EXPECT_EQ(Request.Origin, DriverRequestOrigin::DriverAllocatedIRP);
            EXPECT_EQ(Request.Kind, DriverRequestKind::InternalDeviceControl);
            EXPECT_EQ(Request.ControlCode, uint32_t(OwnedIrpInternalIoctl));
            EXPECT_EQ(Request.File, 0u);
            EXPECT_EQ(Request.RequestorProcessID, 0u);
            EXPECT_EQ(Request.IOStatus, ChildStatus);
            EXPECT_EQ(Request.DispatchStatus, Pending ? windows::StatusPending
                                                      : windows::StatusSuccess);
            EXPECT_EQ(Request.Information, 0u);
            EXPECT_TRUE(Request.Output.empty());
          }
        }
        const auto *Outer = scenarioRequest(*Result, 1);
        ASSERT_NE(Outer, nullptr);
        EXPECT_EQ(Outer->DispatchStatus,
                  Nested ? windows::StatusPending : windows::StatusSuccess);
        const auto *Snapshot = scenarioRequest(*Result, 2);
        ASSERT_NE(Snapshot, nullptr);
        ASSERT_EQ(Snapshot->Output.size(),
                  OwnedIrpSnapshotWords * sizeof(uint32_t));
        EXPECT_EQ(word(*Snapshot, OwnedIrpFailures), 0u);
        EXPECT_EQ(word(*Snapshot, OwnedIrpAllocations), 1u);
        EXPECT_EQ(word(*Snapshot, OwnedIrpLowerDispatches),
                  uint32_t(Submitted));
        EXPECT_EQ(word(*Snapshot, OwnedIrpCompletions), uint32_t(Submitted));
        EXPECT_EQ(word(*Snapshot, OwnedIrpFrees),
                  uint32_t(Mode != OwnedIrpHold));
        EXPECT_EQ(word(*Snapshot, OwnedIrpHeld),
                  uint32_t(Mode == OwnedIrpHold));
        EXPECT_EQ(word(*Snapshot, OwnedIrpCancelCalls),
                  uint32_t(Mode == OwnedIrpCancel));
        EXPECT_EQ(word(*Snapshot, OwnedIrpWorkers),
                  uint32_t(Mode == OwnedIrpWorker));
        EXPECT_EQ(word(*Snapshot, OwnedIrpNestedCompletions), uint32_t(Nested));
        EXPECT_EQ(word(*Snapshot, OwnedIrpLastStatus), ChildStatus);
        EXPECT_EQ(word(*Snapshot, OwnedIrpLastPending), uint32_t(Pending));
        EXPECT_EQ(word(*Snapshot, OwnedIrpLastDeviceIsUpper),
                  uint32_t(Submitted && CallerStack));
        EXPECT_EQ(word(*Snapshot, OwnedIrpDispatchReturns),
                  uint32_t(Submitted));
        EXPECT_EQ(word(*Snapshot, OwnedIrpLastDispatchStatus),
                  Pending ? windows::StatusPending : windows::StatusSuccess);
        if (Mode == OwnedIrpHold) {
          const auto *Released = scenarioRequest(*Result, 4);
          ASSERT_NE(Released, nullptr);
          EXPECT_EQ(word(*Released, OwnedIrpFailures), 0u);
          EXPECT_EQ(word(*Released, OwnedIrpFrees), 1u);
          EXPECT_EQ(word(*Released, OwnedIrpHeld), 0u);
          EXPECT_EQ(word(*Released, OwnedIrpCompletions), 1u);
        }
      }
}
#endif

TEST(DriverWDMOwnedIRP, UnsentAllocationDoesNotInventARequest) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpUnsent);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWDMOwnedIRP, CompletionFreesPacketBeforeLowerDispatchReturns) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpInline);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWDMOwnedIRP,
     WorkerCompletionRetainsIndependentOuterAndChildPackets) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpWorker);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWDMOwnedIRP, MoreProcessingRequiredRetainsPacketUntilExplicitFree) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpHold);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWDMOwnedIRP, CancelCompletionReleasesLockAndFreesOnlyTheChild) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpCancel);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWDMOwnedIRP, NestedOuterCompletionPreservesChildCompletionMetadata) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpNested);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}
TEST(DriverWDMOwnedIRP, KernelNeitherBufferRemainsCallerOwnedAfterFree) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  exercise(OwnedIrpKernelBuffer);
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

TEST(DriverWDMOwnedIRP, SequentialSubmissionsKeepIndependentCompletionRecords) {
#ifdef NEVERD_WDM_OWNED_IRP_FIXTURE
  for (const auto *Image : images()) {
    auto Input = options(OwnedIrpInline, false);
    Input.LoadAddress = 0x190000000;
    Input.Requests.resize(1);
    for (uint8_t Mode : {OwnedIrpInline, OwnedIrpWorker, OwnedIrpHold,
                         OwnedIrpCancel, OwnedIrpNested, OwnedIrpUnsent}) {
      auto Submit = control(OwnedIrpSubmitIoctl);
      Submit.Input = {
          Mode, uint8_t(Mode == OwnedIrpWorker || Mode == OwnedIrpCancel)};
      Input.Requests.push_back(std::move(Submit));
      if (Mode == OwnedIrpHold)
        Input.Requests.push_back(control(OwnedIrpReleaseIoctl));
    }
    const size_t SnapshotIndex = Input.Requests.size();
    Input.Requests.push_back(control(OwnedIrpSnapshotIoctl));
    Input.Requests.push_back(file(DriverRequestKind::Cleanup));
    Input.Requests.push_back(file(DriverRequestKind::Close));
    auto Result = emulateDriver(Image, Input);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(Result->UnloadCompleted);
    ASSERT_EQ(Result->Requests.size(), Input.Requests.size() + 5);
    size_t ChildPackets = 0;
    size_t Cancelled = 0;
    for (const auto &Request : Result->Requests) {
      EXPECT_TRUE(Request.Completed);
      if (Request.Origin == DriverRequestOrigin::Scenario) {
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
        continue;
      }
      EXPECT_EQ(Request.Origin, DriverRequestOrigin::DriverAllocatedIRP);
      EXPECT_EQ(Request.Kind, DriverRequestKind::InternalDeviceControl);
      EXPECT_NE(Request.IRP, 0u);
      ++ChildPackets;
      if (Request.IOStatus == framework::RequestCancelled)
        ++Cancelled;
      else
        EXPECT_EQ(Request.IOStatus, windows::StatusSuccess);
    }
    EXPECT_EQ(ChildPackets, 5u);
    EXPECT_EQ(Cancelled, 1u);
    const auto *Snapshot = scenarioRequest(*Result, SnapshotIndex);
    ASSERT_NE(Snapshot, nullptr);
    ASSERT_EQ(Snapshot->Output.size(),
              OwnedIrpSnapshotWords * sizeof(uint32_t));
    EXPECT_EQ(word(*Snapshot, OwnedIrpFailures), 0u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpAllocations), 6u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpFrees), 6u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpLowerDispatches), 5u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpCompletions), 5u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpHeld), 0u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpWorkers), 1u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpCancelCalls), 1u);
    EXPECT_EQ(word(*Snapshot, OwnedIrpNestedCompletions), 3u);
  }
#else
  GTEST_SKIP() << "NEVERD_WDM_OWNED_IRP_FIXTURE requires a genuine WDK fixture";
#endif
}

} // namespace
} // namespace neverd::emulation
