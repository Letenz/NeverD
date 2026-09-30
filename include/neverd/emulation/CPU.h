//===- CPU.h - Architecture-independent CPU boundary ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CPU_H
#define NEVERD_EMULATION_CPU_H
#include "neverd/emulation/BackendFault.h"
#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ExecutionExit.h"
#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/Registers.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace neverd::emulation {
class AddressSpace;
struct BackendHooks {
  std::function<void(uint64_t, uint32_t)> Instruction;
  std::function<void(uint64_t, uint32_t)> Read;
  std::function<void(uint64_t, uint32_t, uint64_t)> Write;
  std::function<void(uint64_t, uint32_t, const char *)> Fault;
  /// Admit only a modeled, synchronous guest exception. The faulting
  /// instruction is abandoned; the caller must consume the fault and install
  /// a validated guest exception transfer before running again.
  std::function<bool(const BackendFault &)> RecoverableFault;
  std::function<void(uint32_t)> Interrupt;
  std::function<void()> InvalidInstruction;
};
/// An owning CPU snapshot associated with exactly one backend instance.
/// Guest memory and hooks are shared by all contexts and are never rolled back.
/// The snapshot may outlive its backend, but can no longer be used afterward.
class BackendContext final {
public:
  struct Storage {
    virtual ~Storage() = default;
    std::weak_ptr<const void> Owner;
    std::weak_ptr<AddressSpace> Space;
  };
  ~BackendContext() = default;
  BackendContext(BackendContext &&) noexcept = default;
  BackendContext &operator=(BackendContext &&) noexcept = default;
  BackendContext(const BackendContext &) = delete;
  BackendContext &operator=(const BackendContext &) = delete;

private:
  friend class ExecutionBackend;
  explicit BackendContext(std::unique_ptr<Storage> State)
      : State(std::move(State)) {}
  std::unique_ptr<Storage> State;
};
/// CPU execution over an explicitly selected guest ISA and execution contract.
/// Privilege and memory access follow the selected execution contract.
/// This interface supplies no guest OS, ABI or loader.
class ExecutionBackend : public GuestMemory {
public:
  using XmmValue = RegisterValue;
  virtual GuestArchitecture architecture() const = 0;
  std::shared_ptr<AddressSpace> addressSpace() const override = 0;
  llvm::Expected<MemoryView> pinBacking(uint64_t, uint64_t) const override;
  llvm::Error validatePinned(const MemoryView &, uint64_t,
                             uint64_t) const override;
  llvm::Error readPinned(const MemoryView &, uint64_t,
                         llvm::MutableArrayRef<uint8_t>) override;
  llvm::Error writePinned(const MemoryView &, uint64_t,
                          llvm::ArrayRef<uint8_t>) override;
  /// Rebind a stopped CPU to another space backed by the same physical owner.
  /// CPU snapshots remain associated with the space where they were captured.
  virtual llvm::Error bindAddressSpace(std::shared_ptr<AddressSpace> Space) = 0;
  virtual llvm::Expected<RegisterValue> readRegister(CPURegister Register) = 0;
  virtual llvm::Error writeRegister(CPURegister Register,
                                    const RegisterValue &Value) = 0;
  virtual llvm::Error fetch(uint64_t Address,
                            llvm::MutableArrayRef<uint8_t> Bytes) = 0;
  // Typed conveniences preserve architecture identity; cross-ISA access fails.
  llvm::Expected<uint64_t> reg(X64Register Register);
  llvm::Error setReg(X64Register Register, uint64_t Value);
  llvm::Expected<uint64_t> reg(AArch64Register Register);
  llvm::Error setReg(AArch64Register Register, uint64_t Value);
  llvm::Error setGSBase(uint64_t Address);
  llvm::Expected<XmmValue> xmm(unsigned Register);
  llvm::Error setXmm(unsigned Register, const XmmValue &Value);
  llvm::Expected<RegisterValue> vector(unsigned Register);
  llvm::Error setVector(unsigned Register, const RegisterValue &Value);
  virtual llvm::Expected<std::unique_ptr<BackendContext>> saveContext() = 0;
  virtual llvm::Error saveContext(BackendContext &Context) = 0;
  virtual llvm::Error restoreContext(const BackendContext &Context) = 0;
  virtual llvm::Error installHooks(BackendHooks Hooks) = 0;
  /// Execute until stopped, timed out, or faulted. A successful Error result
  /// alone does not imply successful guest completion: inspect fault() and
  /// timedOut() as well, including after an interrupt callback stops execution.
  /// The timeout must be positive and representable by the execution clock;
  /// zero never requests unbounded execution. Invalid budgets have no effects.
  virtual llvm::Error run(uint64_t PC, uint64_t TimeoutMicroseconds);
  /// Typed execution result. Preconditions/setup failures return Error;
  /// outcomes after execution begins return an exit. Recoverable faults remain
  /// pending until takeRecoverableFault(), preserving explicit OS transfer.
  /// Service requests remain pending until takeServiceRequest(); run() reports
  /// these as errors so a legacy caller cannot silently treat them as success.
  /// Built-in CPUs implement this boundary; older external subclasses that
  /// only implement run() reject it explicitly.
  virtual llvm::Expected<ExecutionExit>
  runUntilExit(uint64_t PC, uint64_t TimeoutMicroseconds);
  virtual bool timedOut() const = 0;
  virtual void stop() = 0;
  virtual bool hasMemoryFault() const = 0;
  virtual bool hasDeviceError() const = 0;
  virtual std::optional<BackendFault> fault() const = 0;
  virtual std::optional<BackendFault> takeRecoverableFault() = 0;
  /// Inspect an intercepted service request without releasing the CPU. A
  /// pending request prevents run, state mutation, rebind and CPU snapshots.
  virtual std::optional<ServiceRequest> pendingServiceRequest() const {
    return std::nullopt;
  }
  /// Consume exactly once while stopped. This only releases the pending event;
  /// it does not advance PC or execute a service. The owner must explicitly
  /// handle the request before resuming. Unsupported CPUs return no request.
  virtual std::optional<ServiceRequest> takeServiceRequest() {
    return std::nullopt;
  }
  virtual bool executable(uint64_t Address) const = 0;

protected:
  static std::unique_ptr<BackendContext>
  makeContext(std::unique_ptr<BackendContext::Storage> State) {
    return std::unique_ptr<BackendContext>(
        new BackendContext(std::move(State)));
  }
  static BackendContext::Storage *contextStorage(BackendContext &Context) {
    return Context.State.get();
  }
  static const BackendContext::Storage *
  contextStorage(const BackendContext &Context) {
    return Context.State.get();
  }
};
struct BackendSelection {
  std::unique_ptr<ExecutionBackend> CPU;
  ExecutionBackendKind Kind;
  std::string Reason;
};
llvm::Expected<BackendSelection>
createExecutionBackend(const ExecutionConfiguration &Configuration,
                       std::shared_ptr<AddressSpace> Space);
llvm::Expected<BackendSelection>
createExecutionBackend(const ExecutionConfiguration &Configuration,
                       uint64_t MemoryLimit);
/// Auto chooses a native adapter only for a matching host/guest ISA and a
/// checked contract. Unavailable native execution is an error, not a silent
/// semantic downgrade. The legacy Windows contract remains software-only.
llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       uint64_t MemoryLimit,
                       GuestArchitecture Architecture = GuestArchitecture::X64);
/// Attach an independent CPU to an existing address space without copying RAM.
llvm::Expected<BackendSelection>
createExecutionBackend(ExecutionBackendKind Kind, ExecutionContract Contract,
                       std::shared_ptr<AddressSpace> Space,
                       GuestArchitecture Architecture = GuestArchitecture::X64);
} // namespace neverd::emulation
#endif
