//===- KvmRunTests.cpp - Host interruption and deadline regression --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/kvm/KvmVM.h"
#include "gtest/gtest.h"

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <signal.h>
#include <thread>

namespace {
#define NEVERD_KVM_TEST_VALUE(Name, Value) constexpr unsigned Name = Value;
#define NEVERD_KVM_TEST_STATE_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#define NEVERD_KVM_TEST_TEXT(Name, Value) constexpr char Name[] = Value;
#include "KvmRunCases.def"
#undef NEVERD_KVM_TEST_TEXT
#undef NEVERD_KVM_TEST_STATE_VALUE
#undef NEVERD_KVM_TEST_VALUE
enum class RunResult { Interrupted, Transient, Fatal, UntilKick };
RunResult Result;
unsigned Calls;
std::atomic<bool> Stop;
bool StopOnEntry;
bool RejectMask;
int KickSignal;
std::atomic<bool> Entered;
std::thread::id EntryThread;
} // namespace

// Only this test executable wraps ioctl. KvmVM has no live descriptors here,
// so a runner does not need /dev/kvm to exercise the host-entry retry contract.
extern "C" int __wrap_ioctl(int, unsigned long Request, ...) {
  if (Request == KVM_SET_SIGNAL_MASK) {
    if (RejectMask) {
      errno = EINVAL;
      return -1;
    }
    va_list Args;
    va_start(Args, Request);
    const auto *Mask = va_arg(Args, const kvm_signal_mask *);
    uint64_t Bits = 0;
    EXPECT_EQ(Mask->len, sizeof(Bits));
    std::memcpy(&Bits, Mask->sigset, sizeof(Bits));
    va_end(Args);
    for (int Signal = SIGRTMIN; Signal <= SIGRTMAX; ++Signal)
      if (!(Bits & (uint64_t(1) << (Signal - 1))))
        KickSignal = Signal;
    EXPECT_NE(KickSignal, 0);
    return 0;
  }
  EXPECT_EQ(Request, KVM_RUN);
  EntryThread = std::this_thread::get_id();
  ++Calls;
  Entered = true;
  if (Result == RunResult::UntilKick) {
    sigset_t Pending;
    do {
      sigpending(&Pending);
      std::this_thread::yield();
    } while (!sigismember(&Pending, KickSignal));
    errno = EINTR;
    return -1;
  }
  if (StopOnEntry)
    Stop = true;
  if (Result == RunResult::Fatal) {
    errno = EIO;
    return -1;
  }
  if (Result == RunResult::Interrupted || Calls <= TransientInterruptions) {
    errno = EINTR;
    return -1;
  }
  return 0;
}

namespace neverd::emulation {
namespace {
class KvmRun : public ::testing::Test {
protected:
  KvmVM VM;
  void SetUp() override {
    Calls = 0;
    Stop = false;
    StopOnEntry = false;
    RejectMask = false;
    KickSignal = 0;
    Entered = false;
    Result = RunResult::Transient;
    ASSERT_EQ(llvm::toString(VM.initializeRunControl()), "");
  }
  llvm::Error run(unsigned Microseconds) {
    return VM.runUntilExit({std::chrono::steady_clock::now() +
                                std::chrono::microseconds(Microseconds),
                            &Stop});
  }
};

TEST_F(KvmRun, RetriesTransientHostInterruptions) {
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Calls, TransientInterruptions + 1);
}

TEST_F(KvmRun, RepeatedInterruptionsRespectDeadline) {
  Result = RunResult::Interrupted;
  EXPECT_EQ(llvm::toString(run(InterruptedTimeoutMicroseconds)),
            diagnostic::KvmRun);
}

TEST_F(KvmRun, FatalHostErrorDoesNotRetry) {
  Result = RunResult::Fatal;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 1u);
}
TEST_F(KvmRun, FatalHostErrorOutranksASimultaneousStop) {
  Result = RunResult::Fatal;
  StopOnEntry = true;
  auto E = run(TimeoutMicroseconds);
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::KvmRun);
  EXPECT_TRUE(Stop);
  EXPECT_EQ(Calls, 1u);
}

TEST_F(KvmRun, ExpiredDeadlineDoesNotEnterCPU) {
  EXPECT_EQ(llvm::toString(run(0)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 0u);
}

TEST_F(KvmRun, StopBeforeEntryDoesNotEnterCPU) {
  Stop = true;
  auto E = run(TimeoutMicroseconds);
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 0u);
}

TEST_F(KvmRun, StopDuringHostInterruptionDoesNotRetry) {
  StopOnEntry = true;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 1u);
}

TEST_F(KvmRun, DeadlineInterruptsAnActiveEntryAndDoesNotPoisonTheNextRun) {
  Result = RunResult::UntilKick;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRun);
  EXPECT_TRUE(Entered);
  EXPECT_EQ(Calls, 1u);
  Result = RunResult::Transient;
  Calls = 0;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Calls, TransientInterruptions + 1);
}

TEST_F(KvmRun, StopInterruptsAnActiveEntryWithADistantDeadline) {
  Result = RunResult::UntilKick;
  std::atomic<bool> Done{false};
  std::thread Request([&] {
    while (!Done && !Entered.load())
      std::this_thread::yield();
    Stop = true;
  });
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRun);
  Done = true;
  Request.join();
  EXPECT_EQ(Calls, 1u);
}

TEST_F(KvmRun, FailedWorkerInitializationJoinsBeforeReturning) {
  RejectMask = true;
  EXPECT_EQ(llvm::toString(VM.initializeRunControl()),
            diagnostic::KvmRunControl);
  RejectMask = false;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
}

TEST_F(KvmRun, ConcurrentEntriesCannotReplaceTheBorrowedControl) {
  Result = RunResult::UntilKick;
  std::atomic<bool> Done{false};
  std::string Outcome;
  std::thread First([&] {
    Outcome = llvm::toString(run(TimeoutMicroseconds));
    Done = true;
  });
  while (!Done && !Entered)
    std::this_thread::yield();
  EXPECT_TRUE(Entered);
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRunActive);
  Stop = true;
  First.join();
  EXPECT_EQ(Outcome, diagnostic::KvmRun);
  EXPECT_EQ(Calls, 1u);
}

TEST_F(KvmRun, IgnoredCancellationSignalRejectsEntryWithoutChangingItsHandler) {
  struct sigaction Ignored{}, Previous{};
  Ignored.sa_handler = SIG_IGN;
  sigemptyset(&Ignored.sa_mask);
  ASSERT_EQ(sigaction(KickSignal, &Ignored, &Previous), 0);
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRunSignal);
  EXPECT_EQ(Calls, 0u);
  struct sigaction Actual{};
  EXPECT_EQ(sigaction(KickSignal, nullptr, &Actual), 0);
  EXPECT_EQ(Actual.sa_handler, SIG_IGN);
  ASSERT_EQ(sigaction(KickSignal, &Previous, nullptr), 0);
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
}

TEST_F(KvmRun, StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries) {
  std::thread::id PrepareThread, CaptureThread;
  unsigned Prepared = 0, Captured = 0;
  auto E = VM.runUntilExit(
      {std::chrono::steady_clock::now() +
           std::chrono::microseconds(TimeoutMicroseconds),
       &Stop},
      [&] {
        PrepareThread = std::this_thread::get_id();
        ++Prepared;
        return llvm::Error::success();
      },
      [&] {
        CaptureThread = std::this_thread::get_id();
        ++Captured;
        return llvm::Error::success();
      });
  EXPECT_EQ(llvm::toString(std::move(E)), "");
  EXPECT_EQ(Prepared, 1u);
  EXPECT_EQ(Captured, 1u);
  EXPECT_EQ(Calls, TransientInterruptions + 1);
  EXPECT_EQ(PrepareThread, EntryThread);
  EXPECT_EQ(CaptureThread, EntryThread);
  EXPECT_NE(EntryThread, std::this_thread::get_id());
}

TEST_F(KvmRun, FailedPreparationDoesNotEnterOrCaptureOrPoisonTheNextRun) {
  unsigned Prepared = 0, Captured = 0;
  auto E = VM.runUntilExit(
      {std::chrono::steady_clock::now() +
           std::chrono::microseconds(TimeoutMicroseconds),
       &Stop},
      [&] {
        ++Prepared;
        return diagnostic::error(PrepareFailure);
      },
      [&] {
        ++Captured;
        return llvm::Error::success();
      });
  EXPECT_EQ(llvm::toString(std::move(E)), PrepareFailure);
  EXPECT_EQ(Calls, 0u);
  EXPECT_EQ(Captured, 0u);
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Prepared, 1u);
  EXPECT_EQ(Captured, 0u);
}

TEST_F(KvmRun, SequentialEntriesReuseWorkerWithoutRetainingPriorTransfers) {
  std::array<uint64_t, TransferEntries> Packets{};
  std::array<unsigned, TransferEntries> Prepared{}, Captured{};
  std::thread::id Worker;
  const MachineRunControl Control{
      std::chrono::steady_clock::now() +
          std::chrono::microseconds(TimeoutMicroseconds),
      &Stop};
  for (unsigned I = 0; I < Packets.size(); ++I) {
    auto E = VM.runUntilExit(
        Control,
        [&] {
          if (!I)
            Worker = std::this_thread::get_id();
          EXPECT_EQ(std::this_thread::get_id(), Worker);
          ++Prepared[I];
          Packets[I] = TransferInitialValue + I;
          return llvm::Error::success();
        },
        [&] {
          EXPECT_EQ(std::this_thread::get_id(), Worker);
          ++Captured[I];
          Packets[I] += TransferIncrement;
          return llvm::Error::success();
        });
    ASSERT_EQ(llvm::toString(std::move(E)), "");
    EXPECT_EQ(EntryThread, Worker);
    EXPECT_NE(Worker, std::this_thread::get_id());
  }
  EXPECT_EQ(Calls, TransientInterruptions + TransferEntries);
  for (unsigned I = 0; I < Packets.size(); ++I) {
    EXPECT_EQ(Prepared[I], 1u);
    EXPECT_EQ(Captured[I], 1u);
    EXPECT_EQ(Packets[I], TransferInitialValue + I + TransferIncrement);
  }
}

TEST_F(KvmRun, FailedCaptureReturnsItsErrorAndReleasesBorrowedTransfers) {
  unsigned Captured = 0;
  auto E = VM.runUntilExit({std::chrono::steady_clock::now() +
                                std::chrono::microseconds(TimeoutMicroseconds),
                            &Stop},
                           {}, [&] {
                             ++Captured;
                             return diagnostic::error(CaptureFailure);
                           });
  EXPECT_EQ(llvm::toString(std::move(E)), CaptureFailure);
  EXPECT_EQ(Calls, TransientInterruptions + 1);
  EXPECT_EQ(Captured, 1u);
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Captured, 1u);
}

TEST_F(KvmRun, StopDuringPreparationSkipsEntryAndCapture) {
  unsigned Captured = 0;
  auto E = VM.runUntilExit(
      {std::chrono::steady_clock::now() +
           std::chrono::microseconds(TimeoutMicroseconds),
       &Stop},
      [&] {
        Stop = true;
        return llvm::Error::success();
      },
      [&] {
        ++Captured;
        return llvm::Error::success();
      });
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 0u);
  EXPECT_EQ(Captured, 0u);
  Stop = false;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Captured, 0u);
}

TEST_F(KvmRun, CancelledActiveEntryRetiresTransfersWithoutCapture) {
  Result = RunResult::UntilKick;
  unsigned Prepared = 0, Captured = 0;
  auto E = VM.runUntilExit(
      {std::chrono::steady_clock::now() +
           std::chrono::microseconds(TimeoutMicroseconds),
       &Stop},
      [&] {
        ++Prepared;
        return llvm::Error::success();
      },
      [&] {
        ++Captured;
        return llvm::Error::success();
      });
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 1u);
  EXPECT_EQ(Prepared, 1u);
  EXPECT_EQ(Captured, 0u);
  Result = RunResult::Transient;
  Calls = 0;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Prepared, 1u);
  EXPECT_EQ(Captured, 0u);
}
TEST_F(KvmRun, CompleteValidatesCapturedPacketOnCallerBeforeCancellation) {
  unsigned Captured = 0, Completed = 0;
  const auto Caller = std::this_thread::get_id();
  auto E = VM.runUntilExit(
      {std::chrono::steady_clock::now() +
           std::chrono::microseconds(TimeoutMicroseconds),
       &Stop},
      {},
      [&] {
        ++Captured;
        Stop = true;
        return llvm::Error::success();
      },
      [&] {
        EXPECT_EQ(std::this_thread::get_id(), Caller);
        EXPECT_NE(std::this_thread::get_id(), EntryThread);
        EXPECT_EQ(Captured, 1u);
        ++Completed;
        return llvm::Error::success();
      });
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::KvmRun);
  EXPECT_EQ(Completed, 1u);
  Stop = false;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Captured, 1u);
  EXPECT_EQ(Completed, 1u);
}
TEST_F(KvmRun, CompletionFailureOutranksStopDuringSuccessfulCapture) {
  unsigned Completed = 0;
  auto E = VM.runUntilExit(
      {std::chrono::steady_clock::now() +
           std::chrono::microseconds(TimeoutMicroseconds),
       &Stop},
      {},
      [&] {
        Stop = true;
        return llvm::Error::success();
      },
      [&] {
        ++Completed;
        return diagnostic::error(CompletionFailure);
      });
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), CompletionFailure);
  EXPECT_EQ(Completed, 1u);
  Stop = false;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Completed, 1u);
}
TEST_F(KvmRun, StopDuringCompletionDoesNotPublishASuccessfulEntry) {
  unsigned Completed = 0;
  auto E = VM.runUntilExit({std::chrono::steady_clock::now() +
                                std::chrono::microseconds(TimeoutMicroseconds),
                            &Stop},
                           {}, {}, [&] {
                             ++Completed;
                             Stop = true;
                             return llvm::Error::success();
                           });
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::KvmRun);
  EXPECT_EQ(Completed, 1u);
  Stop = false;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
  EXPECT_EQ(Completed, 1u);
}
TEST_F(KvmRun, CompletionCannotReplaceTheActiveEntryControl) {
  auto E = VM.runUntilExit({std::chrono::steady_clock::now() +
                                std::chrono::microseconds(TimeoutMicroseconds),
                            &Stop},
                           {}, {}, [&] {
                             const auto Before = Calls;
                             EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)),
                                       diagnostic::KvmRunActive);
                             EXPECT_EQ(Calls, Before);
                             return llvm::Error::success();
                           });
  EXPECT_EQ(llvm::toString(std::move(E)), "");
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), "");
}
TEST_F(KvmRun, HostOrCaptureFailureDoesNotInvokeCompletion) {
  for (const bool HostFailure : {false, true}) {
    SCOPED_TRACE(HostFailure);
    Result = HostFailure ? RunResult::Fatal : RunResult::Transient;
    unsigned Completed = 0;
    auto E = VM.runUntilExit(
        {std::chrono::steady_clock::now() +
             std::chrono::microseconds(TimeoutMicroseconds),
         &Stop},
        {}, [] { return diagnostic::error(CaptureFailure); },
        [&] {
          ++Completed;
          return llvm::Error::success();
        });
    EXPECT_EQ(llvm::toString(std::move(E)),
              HostFailure ? diagnostic::KvmRun : CaptureFailure);
    EXPECT_EQ(Completed, 0u);
  }
}
} // namespace
} // namespace neverd::emulation
