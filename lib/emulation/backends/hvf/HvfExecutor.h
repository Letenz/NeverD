//===- HvfExecutor.h - Process VM and vCPU thread ownership ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_HVFEXECUTOR_H
#define NEVERD_EMULATION_HVFEXECUTOR_H

#include "../../core/MachineRunControl.h"
#include "../../core/MemoryProjection.h"

#if defined(__APPLE__) && defined(NEVERD_EMULATION_HVF)
#include "llvm/ADT/STLFunctionalExtras.h"

#include <Hypervisor/Hypervisor.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace neverd::emulation::hvf {
#if defined(__arm64__)
using Cpu = hv_vcpu_t;
#else
using Cpu = hv_vcpuid_t;
#endif

llvm::Error error(const char *Operation, hv_return_t Status);
llvm::Error unavailable(const char *Operation, hv_return_t Status);

/// Every native call except ARM64's asynchronous interrupt runs on Worker. The
/// submitting thread retains the core execution lease; actions must never
/// acquire a core lock or call guest observers on this worker.
class Executor final {
public:
  using Action = llvm::function_ref<llvm::Error(Executor &)>;
  static llvm::Expected<std::shared_ptr<Executor>> acquire();
  ~Executor();
  llvm::Error execute(const void *Identity,
                      const std::array<MemoryRegistration, 2> &Memory,
                      MachineRunControl Control, Action Run);
  void detach(const void *Identity);
  Cpu cpu() const { return CPU; }
#if defined(__arm64__)
  const hv_vcpu_exit_t &exit() const { return *Exit; }
#endif
  /// Acknowledges ARM64's watchdog or Intel's owner-thread finite deadline
  /// before returning. An interrupted entry retires its vCPU before another job
  /// enters.
  using Completion = llvm::function_ref<llvm::Error(bool Cancelled)>;
  llvm::Error run(MachineRunControl Control, Completion Complete);

private:
  Executor();
  struct Request;
  llvm::Error submit(Action Run, const MachineRunControl *Control = nullptr);
  void work();
  llvm::Error initialize();
  llvm::Error createCPU();
  llvm::Error destroyCPU();
  llvm::Error unmap();
  void retireVM();
  llvm::Error bind(const void *Identity,
                   const std::array<MemoryRegistration, 2> &Memory);
  struct RunControl;
  std::unique_ptr<RunControl> Deadline;
  // A dying executor and its replacement cannot own the process VM together.
  std::unique_lock<std::mutex> VMLease;
  bool VMCreated = false, CPUCreated = false;
  Cpu CPU{};
#if defined(__arm64__)
  hv_vcpu_exit_t *Exit = nullptr;
#endif
  const void *Active = nullptr;
  std::array<MemoryRegistration, 2> Mappings{};
  unsigned Mapped = 0;
  std::string Failure;
  std::timed_mutex Admission;
  std::mutex Mutex;
  std::condition_variable Changed;
  Request *Pending = nullptr;
  bool Shutdown = false;
  std::thread Worker;
};

/// Detach synchronously while the CPU's projection and physical owner live.
class Binding final {
public:
  explicit Binding(std::shared_ptr<Executor> Host, MemoryProjection &Memory)
      : Host(std::move(Host)), Memory(Memory.registrations()) {}
  Binding(const Binding &) = delete;
  Binding &operator=(const Binding &) = delete;
  ~Binding() { Host->detach(this); }
  llvm::Error execute(MachineRunControl Control, Executor::Action Run) {
    return Host->execute(this, Memory, Control, Run);
  }

private:
  std::shared_ptr<Executor> Host;
  std::array<MemoryRegistration, 2> Memory;
};
} // namespace neverd::emulation::hvf
#endif
#endif
