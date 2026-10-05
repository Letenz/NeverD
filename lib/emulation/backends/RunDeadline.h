//===- RunDeadline.h - Acknowledged native cancellation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_RUNDEADLINE_H
#define NEVERD_EMULATION_RUNDEADLINE_H

#include "../core/ExecutionLimits.h"
#include "../core/MachineRunControl.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace neverd::emulation {
template <typename Status> struct NativeEntryResult {
  std::optional<Status> Value;
  bool Cancelled = false;
};
/// A transport-owned worker, reused across entries. The interrupt operation
/// must be nonthrowing and remain valid until this controller is destroyed.
/// Disarming waits for any in-flight interrupt call, so an old generation
/// cannot cancel the next entry. Repeated requests close the race where the
/// worker reaches a deadline before the owning thread enters the host API.
template <typename Interrupt> class RunDeadline final {
  static_assert(std::is_nothrow_invocable_v<Interrupt &>);

public:
  explicit RunDeadline(Interrupt Cancel)
      : Cancel(std::move(Cancel)), Worker([this] { wait(); }) {}
  ~RunDeadline() {
    {
      std::lock_guard Lock(Mutex);
      Shutdown = true;
      Changed.notify_one();
    }
    Worker.join();
  }
  RunDeadline(const RunDeadline &) = delete;
  RunDeadline &operator=(const RunDeadline &) = delete;

  /// Execute one nonthrowing host API call under this borrowed control. An
  /// absent result means cancellation rejected entry before calling the host.
  /// Retain an actual host result even with concurrent cancellation; only the
  /// transport can interpret its success/failure convention and priority.
  template <typename HostCall>
  auto invoke(MachineRunControl Control, HostCall &&Call) {
    static_assert(std::is_nothrow_invocable_v<HostCall &>);
    using Status = std::invoke_result_t<HostCall &>;
    if (Control.interrupted())
      return NativeEntryResult<Status>{std::nullopt, true};
    arm(Control);
    // Arming may wait for an older interrupt to retire. Recheck before entry.
    if (Control.interrupted()) {
      disarm();
      return NativeEntryResult<Status>{std::nullopt, true};
    }
    auto Value = Call();
    const bool Cancelled = disarm();
    return NativeEntryResult<Status>{std::move(Value),
                                     Cancelled || Control.interrupted()};
  }

  void arm(MachineRunControl Control) {
    std::lock_guard Lock(Mutex);
    this->Control = Control;
    Armed = true;
    CancelIssued = false;
    ++Generation;
    Changed.notify_one();
  }
  bool disarm() {
    std::lock_guard Lock(Mutex);
    Armed = false;
    Control.Stop = nullptr;
    Changed.notify_one();
    return CancelIssued;
  }

private:
  void wait() {
    using Clock = std::chrono::steady_clock;
    constexpr auto Poll =
        std::chrono::microseconds(execution_limits::CancelRetryMicroseconds);
    std::unique_lock Lock(Mutex);
    while (!Shutdown) {
      Changed.wait(Lock, [&] { return Shutdown || Armed; });
      if (Shutdown)
        break;
      const auto CurrentGeneration = Generation;
      const auto Now = Clock::now();
      // A stop token is owned by the active CPU. Polling it also covers a stop
      // requested just before host entry, without making core depend on a
      // concrete transport's interrupt API.
      if (!CancelIssued && (Now >= Control.Deadline || Control.stopRequested()))
        CancelIssued = true;
      if (CancelIssued)
        Cancel();
      const auto Wake = CancelIssued   ? Now + Poll
                        : Control.Stop ? std::min(Now + Poll, Control.Deadline)
                                       : Control.Deadline;
      Changed.wait_until(Lock, Wake, [&] {
        return Shutdown || !Armed || Generation != CurrentGeneration;
      });
    }
  }

  Interrupt Cancel;
  std::mutex Mutex;
  std::condition_variable Changed;
  MachineRunControl Control{};
  uint64_t Generation = 0;
  bool Armed = false, Shutdown = false, CancelIssued = false;
  std::thread Worker;
};
} // namespace neverd::emulation
#endif
