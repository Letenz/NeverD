//===- HvfExecutor.cpp - Serialized native execution on macOS --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfExecutor.h"

#if defined(__APPLE__) && defined(NEVERD_EMULATION_HVF)
#include "../../core/ExecutionDiagnostics.h"
#include "../RunDeadline.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

#include <sys/sysctl.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <Hypervisor/hv_vmx.h>
#endif

namespace neverd::emulation::hvf {
namespace {
constexpr char Interrupted[] = "HVF entry interrupted";
std::mutex &vmMutex() {
  static std::mutex Mutex;
  return Mutex;
}
std::string message(const char *Operation, hv_return_t Status) {
  return llvm::formatv("HVF {0} failed ({1:x})", Operation, uint32_t(Status))
      .str();
}
llvm::Error runError(Cpu CPU, hv_return_t Status) {
#if defined(__x86_64__)
  auto Text = message("hv_vcpu_run_until", Status);
  // HV_ERROR alone does not distinguish invalid VM-entry controls from
  // invalid guest state. Read diagnostics on the owner before any retirement.
  // VM-instruction error can predate this entry, even on a successful host;
  // this snapshot is supporting evidence rather than an attributed cause.
  const std::pair<const char *, uint32_t> Fields[] = {
      {"instruction_error", VMCS_RO_INSTR_ERROR},
      {"exit_reason", VMCS_RO_EXIT_REASON},
      {"pin_controls", VMCS_CTRL_PIN_BASED},
      {"cpu_controls", VMCS_CTRL_CPU_BASED},
      {"secondary_controls", VMCS_CTRL_CPU_BASED2},
      {"entry_controls", VMCS_CTRL_VMENTRY_CONTROLS},
      {"exit_controls", VMCS_CTRL_VMEXIT_CONTROLS},
      {"cr0", VMCS_GUEST_CR0},
      {"cr3", VMCS_GUEST_CR3},
      {"cr4", VMCS_GUEST_CR4},
      {"efer", VMCS_GUEST_IA32_EFER},
      {"rip", VMCS_GUEST_RIP},
      {"rflags", VMCS_GUEST_RFLAGS},
      {"cs_access", VMCS_GUEST_CS_AR},
      {"ss_access", VMCS_GUEST_SS_AR}};
  for (auto [Name, Field] : Fields) {
    uint64_t Value = 0;
    if (auto S = hv_vmx_vcpu_read_vmcs(CPU, Field, &Value))
      Text +=
          llvm::formatv("; {0} read failed ({1:x})", Name, uint32_t(S)).str();
    else
      Text += llvm::formatv("; {0}={1:x}", Name, Value).str();
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
#else
  return error("hv_vcpu_run", Status);
#endif
}
} // namespace
llvm::Error error(const char *Operation, hv_return_t Status) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 message(Operation, Status));
}
llvm::Error unavailable(const char *Operation, hv_return_t Status) {
  const auto Kind = Status == HV_DENIED ? BackendAvailability::DeviceAccess
                    : Status == HV_UNSUPPORTED
                        ? BackendAvailability::MissingCapability
                        : BackendAvailability::InitializationFailed;
  auto Text = message(Operation, Status);
  if (Status == HV_DENIED)
    Text += "; sign the host executable with com.apple.security.hypervisor";
  return llvm::make_error<BackendUnavailableError>(Text, Kind);
}

struct Executor::Request {
  Action Run;
  llvm::Error Result = llvm::Error::success();
  bool Done = false;
};
struct Executor::RunControl {
  struct Interrupt {
    Executor &Host;
    std::atomic<hv_return_t> Status{HV_SUCCESS};
    void operator()() noexcept {
#if defined(__arm64__)
      const auto Result = hv_vcpus_exit(&Host.CPU, 1);
#else
      const auto Result = hv_vcpu_interrupt(&Host.CPU, 1);
#endif
      if (Result != HV_SUCCESS)
        Status.store(Result);
    }
  };
  Interrupt Cancel;
  struct Forward {
    Interrupt *Cancel;
    void operator()() noexcept { (*Cancel)(); }
  };
  RunDeadline<Forward> Timer;
  explicit RunControl(Executor &Host) : Cancel{Host}, Timer(Forward{&Cancel}) {}
};

Executor::Executor() : Worker([this] { work(); }) {}
llvm::Expected<std::shared_ptr<Executor>> Executor::acquire() {
  static std::mutex RegistryMutex;
  static std::weak_ptr<Executor> Current;
  std::lock_guard Lock(RegistryMutex);
  if (auto Host = Current.lock())
    return Host;
  auto Host = std::shared_ptr<Executor>(new Executor());
  if (auto E = Host->submit([](Executor &H) { return H.initialize(); }))
    return E;
  Current = Host;
  return Host;
}
Executor::~Executor() {
  llvm::consumeError(submit([](Executor &H) -> llvm::Error {
    H.retireVM();
    if (H.VMLease.owns_lock())
      H.VMLease.unlock();
    return llvm::Error::success();
  }));
  {
    std::lock_guard Lock(Mutex);
    Shutdown = true;
    Changed.notify_one();
  }
  Worker.join();
}
llvm::Error Executor::submit(Action Run, const MachineRunControl *Control) {
  std::unique_lock Gate(Admission, std::defer_lock);
  if (Control) {
    // Recheck stop while waiting, including a deadline at time_point::max().
    while (!Gate.try_lock_for(std::chrono::milliseconds(1)))
      if (Control->interrupted())
        return diagnostic::interrupted(Interrupted, *Control);
    if (Control->interrupted())
      return diagnostic::interrupted(Interrupted, *Control);
  } else {
    Gate.lock();
  }
  Request Work{Run};
  std::unique_lock Lock(Mutex);
  Pending = &Work;
  Changed.notify_one();
  Changed.wait(Lock, [&] { return Work.Done; });
  return std::move(Work.Result);
}
void Executor::work() {
  std::unique_lock Lock(Mutex);
  while (true) {
    Changed.wait(Lock, [&] { return Shutdown || Pending; });
    if (Shutdown)
      return;
    auto *Work = Pending;
    Lock.unlock();
    auto Result = Work->Run(*this);
    Lock.lock();
    Work->Result = std::move(Result);
    Pending = nullptr;
    Work->Done = true;
    Changed.notify_all();
  }
}
llvm::Error Executor::initialize() {
  if (__builtin_available(macOS 11.0, *)) {
    // The adapter uses only the macOS 11 API baseline.
  } else
    return diagnostic::unavailable("HVF requires macOS 11 or later",
                                   BackendAvailability::HostAPI);
  int Translated = 0;
  size_t Size = sizeof(Translated);
  if (sysctlbyname("sysctl.proc_translated", &Translated, &Size, nullptr, 0) ==
          0 &&
      Translated)
    return diagnostic::unavailable(
        "HVF requires a native host executable; use the arm64 build",
        BackendAvailability::HostISAMismatch);
  int Supported = 0;
  Size = sizeof(Supported);
  if (sysctlbyname("kern.hv_support", &Supported, &Size, nullptr, 0) != 0 ||
      !Supported)
    return diagnostic::unavailable("HVF is not available on this host",
                                   BackendAvailability::MissingCapability);
  VMLease = std::unique_lock(vmMutex());
#if defined(__arm64__)
  const auto Status = hv_vm_create(nullptr);
#else
  const auto Status = hv_vm_create(HV_VM_DEFAULT);
#endif
  if (Status != HV_SUCCESS)
    return unavailable("hv_vm_create", Status);
  VMCreated = true;
  if (auto E = createCPU())
    return E;
  Deadline = std::make_unique<RunControl>(*this);
  return llvm::Error::success();
}
llvm::Error Executor::createCPU() {
#if defined(__arm64__)
  const auto Status = hv_vcpu_create(&CPU, &Exit, nullptr);
#else
  const auto Status = hv_vcpu_create(&CPU, HV_VCPU_DEFAULT);
#endif
  if (Status != HV_SUCCESS)
    return unavailable("hv_vcpu_create", Status);
  CPUCreated = true;
  return llvm::Error::success();
}
llvm::Error Executor::destroyCPU() {
  if (!CPUCreated)
    return llvm::Error::success();
  const auto Status = hv_vcpu_destroy(CPU);
  if (Status != HV_SUCCESS)
    return error("hv_vcpu_destroy", Status);
  CPUCreated = false;
  return llvm::Error::success();
}
void Executor::retireVM() {
  Deadline.reset();
  // Releasing guest RAM is safe only after every CPU and its process VM have
  // retired. Continuing after failed native teardown would retain dangling
  // host mappings, so this is a fail-stop boundary.
  if (auto E = destroyCPU())
    llvm::report_fatal_error(llvm::Twine(llvm::toString(std::move(E))));
  if (VMCreated) {
    if (auto S = hv_vm_destroy())
      llvm::report_fatal_error(llvm::Twine(message("hv_vm_destroy", S)));
    VMCreated = false;
  }
  Mapped = 0;
  Active = nullptr;
}
llvm::Error Executor::unmap() {
  while (Mapped) {
    const auto &M = Mappings[Mapped - 1];
    if (auto Status = hv_vm_unmap(M.Physical, M.Size)) {
      // Destroying the VM retires all remaining mappings before the binding
      // owner can release its backing. This executor cannot run again.
      Failure = message("hv_vm_unmap", Status);
      retireVM();
      return error("hv_vm_unmap", Status);
    }
    --Mapped;
  }
  Active = nullptr;
  return llvm::Error::success();
}
llvm::Error Executor::bind(const void *Identity,
                           const std::array<MemoryRegistration, 2> &Memory) {
  if (Active == Identity)
    return llvm::Error::success();
  if (auto E = unmap())
    return E;
  const auto PageSize = uint64_t(sysconf(_SC_PAGESIZE));
  for (const auto &M : Memory)
    if (!M.Size || !PageSize || M.Physical % PageSize ||
        uintptr_t(M.Backing) % PageSize || M.Size % PageSize)
      return diagnostic::unavailable(
          "HVF backing must align to the host page size",
          BackendAvailability::MissingCapability);
  Mappings = Memory;
  Active = Identity;
  for (const auto &M : Memory) {
    const auto Status =
        hv_vm_map(M.Backing, M.Physical, M.Size,
                  HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (Status != HV_SUCCESS)
      return llvm::joinErrors(error("hv_vm_map", Status), unmap());
    ++Mapped;
  }
  Active = Identity;
  return llvm::Error::success();
}
llvm::Error Executor::execute(const void *Identity,
                              const std::array<MemoryRegistration, 2> &Memory,
                              MachineRunControl Control, Action Run) {
  return submit(
      [&](Executor &H) -> llvm::Error {
        if (!H.Failure.empty())
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         H.Failure);
        if (auto E = H.bind(Identity, Memory)) {
          H.Failure = llvm::toString(std::move(E));
          return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                         H.Failure);
        }
        if (Control.interrupted())
          return diagnostic::interrupted(Interrupted, Control);
        return Run(H);
      },
      &Control);
}
void Executor::detach(const void *Identity) {
  llvm::consumeError(submit([&](Executor &H) -> llvm::Error {
    if (H.Active == Identity)
      if (auto E = H.unmap())
        H.Failure = llvm::toString(std::move(E));
    return llvm::Error::success();
  }));
}
llvm::Error Executor::run(MachineRunControl Control, Completion Complete) {
  if (!CPUCreated)
    if (auto E = createCPU())
      return E;
  Deadline->Cancel.Status.store(HV_SUCCESS);
  auto Entry = Deadline->Timer.invoke(Control, [&]() noexcept {
#if defined(__arm64__)
    return hv_vcpu_run(CPU);
#else
    return hv_vcpu_run_until(CPU, HV_DEADLINE_FOREVER);
#endif
  });
  llvm::Error Result = llvm::Error::success();
  if (Entry.Value) {
    if (*Entry.Value != HV_SUCCESS)
      Result = runError(CPU, *Entry.Value);
    else
      Result = Complete(Entry.Cancelled);
  }
  if (auto Status = Deadline->Cancel.Status.load(); Status != HV_SUCCESS)
    Result =
        llvm::joinErrors(std::move(Result), error("vcpu interrupt", Status));
  if (Entry.Cancelled) {
    if (auto E = destroyCPU()) {
      Failure = llvm::toString(std::move(E));
      return llvm::joinErrors(
          std::move(Result),
          llvm::createStringError(llvm::inconvertibleErrorCode(), Failure));
    }
    if (auto E = createCPU()) {
      Failure = llvm::toString(std::move(E));
      return llvm::joinErrors(
          std::move(Result),
          llvm::createStringError(llvm::inconvertibleErrorCode(), Failure));
    }
  }
  // A real host/capture error or authenticated CPU exception takes priority
  // over cancellation. Successful state is published by the caller only.
  if (Result)
    return Result;
  if (Entry.Cancelled || Control.interrupted())
    return diagnostic::interrupted(Interrupted, Control);
  return llvm::Error::success();
}

} // namespace neverd::emulation::hvf
#endif
