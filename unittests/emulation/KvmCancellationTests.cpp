//===- KvmCancellationTests.cpp - Interrupt an actually non-exiting vCPU
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "backends/kvm/KvmVM.h"
#include "gtest/gtest.h"

#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <thread>

namespace neverd::emulation {
namespace {
#define NEVERD_KVM_CANCEL_VALUE(Name, Value) constexpr unsigned Name = Value;
#define NEVERD_KVM_CANCEL_BYTES(Name, ...)                                     \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_KVM_CANCEL_TEXT(Name, Text) constexpr char Name[] = Text;
#include "KvmCancellationCases.def"
#undef NEVERD_KVM_CANCEL_VALUE
#undef NEVERD_KVM_CANCEL_BYTES
#undef NEVERD_KVM_CANCEL_TEXT
volatile sig_atomic_t HostSignals = 0;
void hostSignal(int) { HostSignals = HostSignals + 1; }

class KvmCancellation : public testing::Test {
protected:
  std::unique_ptr<KvmVM> VM;
  uint8_t *Memory = nullptr;
  std::atomic<bool> Stop{false};
  sigset_t PreviousMask{}, KickSet{};
  struct sigaction PreviousAction{};
  bool SignalConfigured = false, MaskConfigured = false;

  void SetUp() override {
#if !defined(__x86_64__)
    GTEST_SKIP() << RequiresX64;
#else
    HostSignals = 0;
    struct sigaction Action{};
    Action.sa_handler = hostSignal;
    sigemptyset(&Action.sa_mask);
    ASSERT_EQ(sigaction(SIGRTMAX, &Action, &PreviousAction), 0);
    SignalConfigured = true;
    sigemptyset(&KickSet);
    sigaddset(&KickSet, SIGRTMAX);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &KickSet, &PreviousMask), 0);
    MaskConfigured = true;
    void *Mapping = mmap(nullptr, PageSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(Mapping, MAP_FAILED);
    Memory = static_cast<uint8_t *>(Mapping);
    VM = std::make_unique<KvmVM>();
    const MemoryRegistration Registration{0, Memory, PageSize};
    auto E = VM->initialize(llvm::ArrayRef(&Registration, 1));
    if (E) {
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    kvm_sregs Segments{};
    ASSERT_EQ(ioctl(VM->CPU, KVM_GET_SREGS, &Segments), 0);
    Segments.cs.base = Segments.cs.selector = 0;
    ASSERT_EQ(ioctl(VM->CPU, KVM_SET_SREGS, &Segments), 0);
#endif
  }
  void TearDown() override {
    VM.reset();
    if (Memory)
      munmap(Memory, PageSize);
    // Test-installed handlers stay live while restoring the original mask.
    if (MaskConfigured)
      EXPECT_EQ(pthread_sigmask(SIG_SETMASK, &PreviousMask, nullptr), 0);
    if (SignalConfigured)
      EXPECT_EQ(sigaction(SIGRTMAX, &PreviousAction, nullptr), 0);
  }
  void prepare(llvm::ArrayRef<uint8_t> Code) {
#if defined(__x86_64__)
    std::memset(Memory, 0, PageSize);
    std::copy(Code.begin(), Code.end(), Memory);
    kvm_regs Registers{};
    Registers.rflags = InitialFlags;
    ASSERT_EQ(ioctl(VM->CPU, KVM_SET_REGS, &Registers), 0);
#endif
  }
  bool entered() {
    // Memory is written by the actual vCPU, not a C++ worker function.
    return *reinterpret_cast<volatile uint16_t *>(Memory + MarkerOffset) ==
           MarkerValue;
  }
  llvm::Error run(unsigned Microseconds) {
    return VM->runUntilExit({std::chrono::steady_clock::now() +
                                 std::chrono::microseconds(Microseconds),
                             &Stop});
  }
  void checkHostSignalState() {
    sigset_t CurrentMask;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &CurrentMask), 0);
    for (int Signal = 1; Signal < NSIG; ++Signal)
      EXPECT_EQ(sigismember(&CurrentMask, Signal),
                Signal == SIGRTMAX ? 1 : sigismember(&PreviousMask, Signal));
    struct sigaction Action{};
    ASSERT_EQ(sigaction(SIGRTMAX, nullptr, &Action), 0);
    EXPECT_EQ(Action.sa_handler, hostSignal);
    EXPECT_EQ(HostSignals, 0);
  }
};

TEST_F(KvmCancellation, DeadlineInterruptsANonExitingNativeVCPU) {
  prepare(Loop);
  EXPECT_EQ(llvm::toString(run(DeadlineMicroseconds)), diagnostic::KvmRun);
  EXPECT_TRUE(entered());
  checkHostSignalState();
  prepare(Halt);
  EXPECT_EQ(llvm::toString(run(DeadlineMicroseconds)), "");
  EXPECT_EQ(VM->Run->exit_reason, KVM_EXIT_HLT);
}

TEST_F(KvmCancellation, StopAcknowledgesEachEntryBeforeItCanResumeOrRetire) {
  for (unsigned I = 0; I < ResumeCount; ++I) {
    SCOPED_TRACE(I);
    prepare(Loop);
    Stop = false;
    std::atomic<bool> Done{false};
    std::thread Request([&] {
      while (!Done && !entered())
        std::this_thread::yield();
      Stop = true;
    });
    EXPECT_EQ(llvm::toString(run(StopMicroseconds)), diagnostic::KvmRun);
    Done = true;
    Request.join();
    EXPECT_TRUE(entered());
    checkHostSignalState();
    Stop = false;
    prepare(Halt);
    EXPECT_EQ(llvm::toString(run(DeadlineMicroseconds)), "");
    EXPECT_EQ(VM->Run->exit_reason, KVM_EXIT_HLT);
  }
}

TEST_F(KvmCancellation, PrivateCancellationLeavesApplicationSignalsPending) {
  prepare(Loop);
  std::atomic<bool> Done{false};
  std::thread Request([&] {
    while (!Done && !entered())
      std::this_thread::yield();
    EXPECT_EQ(kill(getpid(), SIGRTMAX), 0);
    Stop = true;
  });
  EXPECT_EQ(llvm::toString(run(StopMicroseconds)), diagnostic::KvmRun);
  Done = true;
  Request.join();
  EXPECT_TRUE(entered());
  checkHostSignalState();
  const timespec Zero{};
  siginfo_t Info{};
  ASSERT_EQ(sigtimedwait(&KickSet, &Info, &Zero), SIGRTMAX);
  EXPECT_EQ(Info.si_code, SI_USER);
  EXPECT_EQ(Info.si_pid, getpid());
}
} // namespace
} // namespace neverd::emulation
