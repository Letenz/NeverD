//===- KvmRunTests.cpp - Host interruption and deadline regression --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/kvm/KvmVM.h"
#include "gtest/gtest.h"

namespace {
#define NEVERD_KVM_TEST_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "KvmRunCases.def"
#undef NEVERD_KVM_TEST_VALUE
enum class RunResult { Interrupted, Transient, Fatal };
RunResult Result;
unsigned Calls;
std::atomic<bool> Stop;
bool StopOnEntry;
} // namespace

// Only this test executable wraps ioctl. KvmVM has no live descriptors here,
// so a runner does not need /dev/kvm to exercise the host-entry retry contract.
extern "C" int __wrap_ioctl(int, unsigned long Request, ...) {
  EXPECT_EQ(Request, KVM_RUN);
  ++Calls;
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
    Result = RunResult::Transient;
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

TEST_F(KvmRun, ExpiredDeadlineDoesNotEnterCPU) {
  EXPECT_EQ(llvm::toString(run(0)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 0u);
}

TEST_F(KvmRun, StopBeforeEntryDoesNotEnterCPU) {
  Stop = true;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 0u);
}

TEST_F(KvmRun, StopDuringHostInterruptionDoesNotRetry) {
  StopOnEntry = true;
  EXPECT_EQ(llvm::toString(run(TimeoutMicroseconds)), diagnostic::KvmRun);
  EXPECT_EQ(Calls, 1u);
}
} // namespace
} // namespace neverd::emulation
