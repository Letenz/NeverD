//===- KvmRunControl.cpp - Acknowledged KVM cancellation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KvmRunControl.h"

#if defined(__linux__) && defined(NEVERD_EMULATION_KVM)
#include "../../core/ExecutionDiagnostics.h"

#include "llvm/ADT/ScopeExit.h"

#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <linux/kvm.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>

namespace neverd::emulation {
namespace {
#define NEVERD_KVM_RUN_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "KvmRunControl.def"
#undef NEVERD_KVM_RUN_VALUE
using Clock = std::chrono::steady_clock;
constexpr auto Poll =
    std::chrono::microseconds(execution_limits::CancelRetryMicroseconds);
} // namespace

struct KvmRunControl::State {
  int VCPU = -1, Signal = 0, Status = -1;
  std::mutex Mutex;
  std::condition_variable Changed;
  std::thread Worker;
  MachineRunControl Control{};
  StateTransfer Prepare, Capture;
  llvm::Error TransferError = llvm::Error::success();
  std::atomic<bool> Cancel{false};
  bool Ready = false, Initialized = false, Shutdown = false, Running = false;
  bool Requested = false, Completed = false, PendingKick = false;
  bool EntryInterrupted = false;

  ~State() {
    if (!Worker.joinable())
      return;
    {
      std::lock_guard Lock(Mutex);
      Shutdown = true;
      Changed.notify_all();
    }
    Worker.join();
  }

  bool initialize() {
    Signal = 0;
    sigset_t Blocked;
    sigfillset(&Blocked);
    if (pthread_sigmask(SIG_BLOCK, &Blocked, nullptr))
      return false;
    // An ignored signal cannot reliably interrupt KVM. Use a non-ignored RT
    // signal, delivered only to this private thread. No handler is installed.
    for (int Candidate = std::min(SIGRTMAX, int(SignalSetBytes * CHAR_BIT));
         Candidate >= SIGRTMIN; --Candidate) {
      struct sigaction Action{};
      if (sigaction(Candidate, nullptr, &Action) ||
          Action.sa_handler == SIG_IGN)
        continue;
      Signal = Candidate;
      break;
    }
    if (!Signal)
      return false;
    struct {
      uint32_t Length;
      uint8_t Bits[SignalSetBytes];
    } Mask{SignalSetBytes, {}};
    static_assert(offsetof(decltype(Mask), Bits) == sizeof(kvm_signal_mask));
    const uint64_t Bits = ~(uint64_t(1) << (Signal - 1));
    std::memcpy(Mask.Bits, &Bits, sizeof(Bits));
    return ioctl(VCPU, KVM_SET_SIGNAL_MASK, &Mask) == 0;
  }

  bool start(std::unique_lock<std::mutex> &Lock) {
    Ready = Initialized = false;
    try {
      Worker = std::thread([this] { work(); });
    } catch (const std::system_error &) {
      return false;
    }
    Changed.wait(Lock, [&] { return Ready; });
    if (!Initialized) {
      Lock.unlock();
      Worker.join();
      Lock.lock();
    }
    return Initialized;
  }

  void work() {
    const bool Success = initialize();
    std::unique_lock Lock(Mutex);
    Initialized = Success;
    Ready = true;
    Changed.notify_all();
    if (!Success)
      return;
    while (true) {
      Changed.wait(Lock, [&] { return Shutdown || Requested; });
      if (Shutdown)
        return;
      Requested = false;
      Lock.unlock();
      int Result = -1;
      bool Interrupted = false;
      llvm::Error Error = llvm::Error::success();
      if (Prepare && !Cancel.load() && !Control.stopRequested() &&
          Clock::now() < Control.Deadline)
        Error = Prepare();
      if (!Error) {
        do {
          if (Cancel.load() || Control.stopRequested() ||
              Clock::now() >= Control.Deadline) {
            Result = -1;
            Interrupted = true;
            break;
          }
          Result = ioctl(VCPU, KVM_RUN, 0);
        } while (Result < 0 && errno == EINTR);
        if (Result >= 0 && Capture)
          Error = Capture();
      }
      Lock.lock();
      Status = Result;
      EntryInterrupted = Interrupted;
      TransferError = std::move(Error);
      Completed = true;
      Changed.notify_all();
      // Retiring the private thread discards its pending kick without ever
      // reading a signal queue shared with the application. Ordinary entries
      // reuse the worker; only cancellation requires a fresh one afterward.
      if (Cancel)
        return;
    }
  }
};

KvmRunControl::KvmRunControl() = default;
KvmRunControl::~KvmRunControl() = default;

llvm::Expected<std::unique_ptr<KvmRunControl>> KvmRunControl::create(int VCPU) {
  auto Result = std::unique_ptr<KvmRunControl>(new KvmRunControl);
  Result->Impl = std::make_unique<State>();
  auto &S = *Result->Impl;
  S.VCPU = VCPU;
  std::unique_lock Lock(S.Mutex);
  if (!S.start(Lock))
    return diagnostic::error(diagnostic::KvmRunControl);
  return Result;
}

llvm::Error KvmRunControl::run(MachineRunControl Control) {
  return run(Control, {}, {});
}

llvm::Error KvmRunControl::run(MachineRunControl Control, StateTransfer Prepare,
                               StateTransfer Capture, Completion Complete) {
  auto &S = *Impl;
  std::unique_lock Lock(S.Mutex);
  if (S.Running)
    return diagnostic::error(diagnostic::KvmRunActive);
  if (Control.interrupted())
    return diagnostic::interrupted(diagnostic::KvmRun, Control);
  S.Running = true;
  if (!S.Worker.joinable() && !S.start(Lock)) {
    S.Running = false;
    return diagnostic::error(diagnostic::KvmRunControl);
  }
  struct sigaction Action{};
  if (sigaction(S.Signal, nullptr, &Action) || Action.sa_handler == SIG_IGN) {
    S.Running = false;
    return diagnostic::error(diagnostic::KvmRunSignal);
  }
  S.Control = Control;
  S.Prepare = Prepare;
  S.Capture = Capture;
  S.Cancel = false;
  S.PendingKick = false;
  S.Completed = false;
  S.Requested = true;
  S.Changed.notify_all();
  while (!S.Completed) {
    const auto Now = Clock::now();
    if (Now >= Control.Deadline || Control.stopRequested())
      S.Cancel = true;
    if (S.Cancel && !S.PendingKick)
      S.PendingKick = pthread_kill(S.Worker.native_handle(), S.Signal) == 0;
    // A blocked, pending signal survives an early kick and is observed on
    // KVM entry. Unlike an API cancellation edge, it needs no repeated posts.
    const auto Wake = S.Cancel       ? Now + Poll
                      : Control.Stop ? std::min(Now + Poll, Control.Deadline)
                                     : Control.Deadline;
    S.Changed.wait_until(Lock, Wake, [&] { return S.Completed; });
  }
  if (S.Cancel) {
    // A completed KVM ioctl alone does not retire a late pending signal.
    // Join before releasing the borrowed stop token, vCPU or run mapping.
    Lock.unlock();
    S.Worker.join();
    Lock.lock();
  }
  S.Control.Stop = nullptr;
  S.Prepare = S.Capture = {};
  auto TransferError = std::move(S.TransferError);
  const int Status = S.Status;
  const bool Cancelled = S.Cancel, EntryInterrupted = S.EntryInterrupted;
  Lock.unlock();
  auto Release = llvm::scope_exit([&] {
    std::lock_guard Lock(S.Mutex);
    S.Running = false;
  });
  if (TransferError)
    return TransferError;
  if (Status < 0) {
    if (EntryInterrupted && (Cancelled || Control.interrupted()))
      return diagnostic::interrupted(diagnostic::KvmRun, Control);
    return diagnostic::error(diagnostic::KvmRun);
  }
  // ISA validation stays on the owner thread. A genuine processor exception
  // or malformed capture outranks a stop observed during successful state IO.
  // Running remains true until this callback and result classification finish.
  if (Complete)
    if (auto E = Complete())
      return E;
  if (Cancelled || Control.interrupted())
    return diagnostic::interrupted(diagnostic::KvmRun, Control);
  return llvm::Error::success();
}
} // namespace neverd::emulation
#endif
