//===- HvfOwnerThread.h - Serial lifetimes on one native thread -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_HVFOWNERTHREAD_H
#define NEVERD_EMULATION_HVFOWNERTHREAD_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace neverd::emulation::hvf {

/// Owns a thread, not a VM. Each job borrows one fully initialized Executor
/// until its work loop returns. Callers must end and wait for that lifetime
/// before releasing the Executor; the worker never owns its own shared_ptr.
class OwnerThread final {
public:
  using Generation = uint64_t;
  using Job = std::function<void()>;

  OwnerThread() : Worker([this] { work(); }) {}
  ~OwnerThread() {
    {
      std::lock_guard Lock(Mutex);
      Shutdown = true;
      Changed.notify_all();
    }
    Worker.join();
  }
  OwnerThread(const OwnerThread &) = delete;
  OwnerThread &operator=(const OwnerThread &) = delete;

  Generation start(Job Run) {
    if (!Run)
      throw std::invalid_argument("HVF owner requires a lifetime job");
    std::unique_lock Lock(Mutex);
    // Wait on the submitting thread. Blocking the owner here could prevent
    // the previous Executor from retiring its VM and releasing its lease.
    Changed.wait(Lock, [&] { return Assigned == Completed; });
    if (Assigned == std::numeric_limits<Generation>::max())
      throw std::overflow_error("HVF owner lifetime generation exhausted");
    // All potentially throwing work precedes publication of borrowed state.
    Pending.swap(Run);
    ++Assigned;
    Changed.notify_all();
    return Assigned;
  }

  void wait(Generation Lifetime) {
    std::unique_lock Lock(Mutex);
    Changed.wait(Lock, [&] { return Completed >= Lifetime; });
  }

  // Clients retain this service until their lock member has been destroyed.
  // A separate function-static mutex could die before a late global client.
  std::mutex &vmMutex() { return VM; }

private:
  void work() {
    std::unique_lock Lock(Mutex);
    while (true) {
      Changed.wait(Lock, [&] { return Shutdown || Pending; });
      if (!Pending)
        return;
      Job Run;
      Run.swap(Pending);
      const auto Lifetime = Assigned;
      Lock.unlock();
      Run();
      // Completion includes destruction of all captured borrowed references.
      Run = nullptr;
      Lock.lock();
      Completed = Lifetime;
      Changed.notify_all();
    }
  }

  std::mutex VM, Mutex;
  std::condition_variable Changed;
  Job Pending;
  Generation Assigned = 0, Completed = 0;
  bool Shutdown = false;
  std::thread Worker;
};

} // namespace neverd::emulation::hvf
#endif
