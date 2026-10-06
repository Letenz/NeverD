//===- WhpEntryControlTests.cpp - WHP entry protocol tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#if defined(_WIN32) && defined(NEVERD_EMULATION_WHP)
#include "arch/x86_64/X64Exception.h"
#include "backends/whp/WhpVirtualProcessor.h"
#include "gtest/gtest.h"

#include <atomic>

namespace neverd::emulation {
namespace {
using Clock = std::chrono::steady_clock;
#define NEVERD_RUN_CONTROL_TEST_VALUE(Name, Value) constexpr auto Name = Value;
#include "RunControlCases.def"
#undef NEVERD_RUN_CONTROL_TEST_VALUE
struct FakePartition {
  std::atomic<unsigned> Entries{0}, Cancels{0}, Deletes{0};
  std::atomic<bool> *StopAtReturn = nullptr;
  HRESULT Result = S_OK;
  WHV_RUN_VP_EXIT_REASON Reason = WHvRunVpExitReasonNone;
  bool WaitForCancel = false;
  static HRESULT WINAPI run(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                            VOID *Context, UINT32) {
    EXPECT_EQ(CPU, ProcessorIndex);
    auto &Target = *static_cast<FakePartition *>(Handle);
    ++Target.Entries;
    static_cast<WHV_RUN_VP_EXIT_CONTEXT *>(Context)->ExitReason = Target.Reason;
    if (Target.StopAtReturn)
      *Target.StopAtReturn = true;
    if (Target.WaitForCancel) {
      const auto Deadline =
          Clock::now() + std::chrono::milliseconds(WaitMilliseconds);
      while (!Target.Cancels && Clock::now() < Deadline)
        std::this_thread::yield();
      EXPECT_NE(Target.Cancels.load(), 0u);
    }
    return Target.Result;
  }
  static HRESULT WINAPI cancel(WHV_PARTITION_HANDLE Handle, UINT32 CPU,
                               UINT32) {
    EXPECT_EQ(CPU, ProcessorIndex);
    ++static_cast<FakePartition *>(Handle)->Cancels;
    return S_OK;
  }
  static HRESULT WINAPI destroy(WHV_PARTITION_HANDLE Handle) {
    ++static_cast<FakePartition *>(Handle)->Deletes;
    return S_OK;
  }
};
class WhpEntryControl : public testing::Test {
protected:
  FakePartition Target;
  WhpVirtualProcessor Partition;
  WHV_RUN_VP_EXIT_CONTEXT Exit{};
  void SetUp() override {
    Partition.Partition = &Target;
    Partition.ProcessorIndex = ProcessorIndex;
    Partition.API.WHvRunVirtualProcessor = FakePartition::run;
    Partition.API.WHvCancelRunVirtualProcessor = FakePartition::cancel;
    Partition.API.WHvDeletePartition = FakePartition::destroy;
  }
  void initialize() {
    ASSERT_EQ(llvm::toString(Partition.initializeRunControl()), "");
  }
  void interrupted(llvm::Error E, bool Stopped, bool Expired) {
    bool Seen = false;
    auto Remaining = llvm::handleErrors(
        std::move(E), [&](const MachineInterruptedError &Interrupted) {
          Seen = true;
          EXPECT_EQ(Interrupted.stopRequested(), Stopped);
          EXPECT_EQ(Interrupted.deadlineReached(), Expired);
        });
    EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
    EXPECT_TRUE(Seen);
  }
};
TEST_F(WhpEntryControl, CancellationTargetsTheRetainedNonzeroProcessor) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  Target.StopAtReturn = &Stop;
  Target.WaitForCancel = true;
  Target.Reason = WHvRunVpExitReasonCanceled;
  interrupted(Partition.run(Exit, {Clock::time_point::max(), &Stop}), true,
              false);
  EXPECT_EQ(Target.Entries.load(), 1u);
  EXPECT_NE(Target.Cancels.load(), 0u);
}
TEST_F(WhpEntryControl, UninitializedControlCannotEnterHost) {
  EXPECT_EQ(llvm::toString(Partition.run(Exit, {Clock::time_point::max()})),
            diagnostic::WhpRunControl);
  EXPECT_EQ(Target.Entries.load(), 0u);
}
TEST_F(WhpEntryControl, PreStoppedEntryDoesNotCallHostAndAllowsRetry) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{true};
  interrupted(Partition.run(Exit, {Clock::time_point::max(), &Stop}), true,
              false);
  EXPECT_EQ(Target.Entries.load(), 0u);
  EXPECT_EQ(Target.Cancels.load(), 0u);
  Stop = false;
  EXPECT_EQ(
      llvm::toString(Partition.run(Exit, {Clock::time_point::max(), &Stop})),
      "");
  EXPECT_EQ(Target.Entries.load(), 1u);
}
TEST_F(WhpEntryControl, ExpiredEntryDoesNotCallHost) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  interrupted(Partition.run(Exit, {Clock::time_point::min()}), false, true);
  EXPECT_EQ(Target.Entries.load(), 0u);
  EXPECT_EQ(Target.Cancels.load(), 0u);
}
TEST_F(WhpEntryControl, StopAtSuccessfulReturnRetainsTypedInterruption) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  Target.StopAtReturn = &Stop;
  interrupted(Partition.run(Exit, {Clock::time_point::max(), &Stop}), true,
              false);
  EXPECT_EQ(Target.Entries.load(), 1u);
}
TEST_F(WhpEntryControl, GenuineHostFailureOutranksASimultaneousStop) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  Target.StopAtReturn = &Stop;
  const struct {
    HRESULT Status;
    const char *Message;
  } Cases[] = {
#define NEVERD_WHP_RUN_FAILURE(Status, Message) {Status, Message},
#include "WhpHostFailureCases.def"
#undef NEVERD_WHP_RUN_FAILURE
  };
  unsigned Entries = 0;
  for (const auto &Case : Cases) {
    Stop = false;
    Target.Result = Case.Status;
    auto E = Partition.run(Exit, {Clock::time_point::max(), &Stop});
    EXPECT_FALSE(E.isA<MachineInterruptedError>());
    EXPECT_EQ(llvm::toString(std::move(E)), Case.Message);
    EXPECT_EQ(Target.Entries.load(), ++Entries);
  }
}
TEST_F(WhpEntryControl, CompletedExitValidationRunsOnCallerBeforeStop) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  Target.StopAtReturn = &Stop;
  const auto Caller = std::this_thread::get_id();
  unsigned Completed = 0;
  auto E = Partition.run(Exit, {Clock::time_point::max(), &Stop}, [&] {
    EXPECT_EQ(std::this_thread::get_id(), Caller);
    ++Completed;
    return diagnostic::error(diagnostic::WhpState);
  });
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::WhpState);
  EXPECT_EQ(Completed, 1u);
}
TEST_F(WhpEntryControl, AuthenticatedExceptionRetainsPriorityOverStop) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  Target.StopAtReturn = &Stop;
  // Inject a completion result, not a native CPU exception. Actual transport
  // exception authentication is exercised by NeverDX64ExceptionTests.
  auto E = Partition.run(Exit, {Clock::time_point::max(), &Stop}, [] {
    return llvm::make_error<X64ExceptionError>(X64Exception{
        unsigned(x64::ExceptionVector::Divide), std::nullopt, std::nullopt});
  });
  EXPECT_TRUE(E.isA<X64ExceptionError>());
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  llvm::consumeError(std::move(E));
}
TEST_F(WhpEntryControl, CancelledHostExitCannotInvokeCompletion) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  Target.StopAtReturn = &Stop;
  Target.Reason = WHvRunVpExitReasonCanceled;
  unsigned Completed = 0;
  interrupted(Partition.run(Exit, {Clock::time_point::max(), &Stop},
                            [&] {
                              ++Completed;
                              return llvm::Error::success();
                            }),
              true, false);
  EXPECT_EQ(Completed, 0u);
}
TEST_F(WhpEntryControl, StopDuringCompletionCannotPublishSuccessfulEntry) {
  ASSERT_NO_FATAL_FAILURE(initialize());
  std::atomic<bool> Stop{false};
  unsigned Completed = 0;
  interrupted(Partition.run(Exit, {Clock::time_point::max(), &Stop},
                            [&] {
                              ++Completed;
                              Stop = true;
                              return llvm::Error::success();
                            }),
              true, false);
  EXPECT_EQ(Completed, 1u);
  Stop = false;
  EXPECT_EQ(
      llvm::toString(Partition.run(Exit, {Clock::time_point::max(), &Stop})),
      "");
  EXPECT_EQ(Completed, 1u);
}
} // namespace
} // namespace neverd::emulation
#endif
