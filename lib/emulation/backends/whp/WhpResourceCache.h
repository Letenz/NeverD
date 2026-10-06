//===- WhpResourceCache.h - Logical CPU and native resource lifetimes ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_RESOURCECACHE_H
#define NEVERD_EMULATION_WHP_RESOURCECACHE_H

#include "../../core/ExecutionDiagnostics.h"

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace neverd::emulation {
/// Cache one cooperative VP independently of the logical CPUs. Retiring the
/// old VP and its GPA window before constructing the next discards hidden CPU
/// state and translations. Parallel bindings retain separate VP leases within
/// the same native partition. Explicit CPU state and RAM remain authoritative.
template <typename Resource> class WhpResourceCache final {
public:
  using Factory = std::function<llvm::Expected<std::unique_ptr<Resource>>()>;
  class Lease final {
  public:
    Lease(std::unique_lock<std::timed_mutex> Lock, Resource &Value)
        : Lock(std::move(Lock)), Value(Value) {}
    Lease(Lease &&) = default;
    Resource &operator*() const { return Value; }
    Resource *operator->() const { return &Value; }

  private:
    std::unique_lock<std::timed_mutex> Lock;
    Resource &Value;
  };

  llvm::Expected<Lease> acquire(const void *Owner, const Factory &Create,
                                MachineRunControl Control) {
    using Clock = std::chrono::steady_clock;
    constexpr auto Poll =
        std::chrono::microseconds(execution_limits::CancelRetryMicroseconds);
    std::unique_lock Lock(Mutex, std::defer_lock);
    for (;;) {
      if (Control.interrupted())
        return diagnostic::interrupted(diagnostic::WhpRun, Control);
      if (Lock.try_lock_until(std::min(Control.Deadline, Clock::now() + Poll)))
        break;
    }
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::WhpRun, Control);
    if (ActiveOwner != Owner) {
      // Native callbacks have retired before the previous lease was released.
      // Never overlap cooperative VP windows or retain a partial replacement.
      Active.reset();
      ActiveOwner = nullptr;
      auto Next = Create();
      if (!Next)
        return Next.takeError();
      if (!*Next)
        return diagnostic::error(diagnostic::WhpCreate);
      if (Control.interrupted())
        return diagnostic::interrupted(diagnostic::WhpRun, Control);
      Active = std::move(*Next);
      ActiveOwner = Owner;
    }
    return Lease(std::move(Lock), *Active);
  }

  void retire(const void *Owner) {
    std::lock_guard Lock(Mutex);
    if (ActiveOwner != Owner)
      return;
    Active.reset();
    ActiveOwner = nullptr;
  }

private:
  std::timed_mutex Mutex;
  const void *ActiveOwner = nullptr;
  std::unique_ptr<Resource> Active;
};

/// One cooperative cache per native resource type. All WHP machines bind the
/// common WhpVirtualProcessor lease so ISA factories cannot allocate VP zero
/// twice.
template <typename Resource>
std::shared_ptr<WhpResourceCache<Resource>> sharedWhpResources() {
  static std::mutex Mutex;
  static std::weak_ptr<WhpResourceCache<Resource>> Weak;
  std::lock_guard Lock(Mutex);
  auto Shared = Weak.lock();
  if (!Shared) {
    Shared = std::make_shared<WhpResourceCache<Resource>>();
    Weak = Shared;
  }
  return Shared;
}

/// The factory may borrow this logical machine's memory only while its binding
/// lives. A lease must end before retiring its binding; machine step() keeps
/// that lease through state capture and cancellation acknowledgement.
template <typename Resource> class WhpResourceBinding final {
public:
  using Cache = WhpResourceCache<Resource>;
  explicit WhpResourceBinding(typename Cache::Factory Create,
                              bool Independent = false)
      : Shared(Independent ? std::make_shared<Cache>()
                           : sharedWhpResources<Resource>()),
        Create(std::move(Create)) {}
  ~WhpResourceBinding() { Shared->retire(this); }
  WhpResourceBinding(const WhpResourceBinding &) = delete;
  WhpResourceBinding &operator=(const WhpResourceBinding &) = delete;

  llvm::Expected<typename Cache::Lease> acquire(MachineRunControl Control) {
    return Shared->acquire(this, Create, Control);
  }

private:
  std::shared_ptr<Cache> Shared;
  typename Cache::Factory Create;
};
} // namespace neverd::emulation
#endif
