//===- KvmMachine.cpp - Linux x64 single-step execution ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64ExceptionMonitor.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../arch/x86_64/X64MachineProbe.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../MachineFactories.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#if defined(__linux__) && defined(__x86_64__) && defined(NEVERD_EMULATION_KVM)
#include "KvmVM.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace neverd::emulation {
namespace {
bool sameState(const kvm_segment &L, const kvm_segment &R) {
#define NEVERD_KVM_X64_SEGMENT_FIELD(Field)                                    \
  if (L.Field != R.Field)                                                      \
    return false;
#include "KvmX64State.def"
#undef NEVERD_KVM_X64_SEGMENT_FIELD
  return true;
}
bool sameState(const kvm_dtable &L, const kvm_dtable &R) {
#define NEVERD_KVM_X64_DTABLE_FIELD(Field)                                     \
  if (L.Field != R.Field)                                                      \
    return false;
#include "KvmX64State.def"
#undef NEVERD_KVM_X64_DTABLE_FIELD
  return true;
}
bool sameState(const kvm_sregs &L, const kvm_sregs &R) {
#define NEVERD_KVM_X64_SPECIAL_SEGMENT(Field)                                  \
  if (!sameState(L.Field, R.Field))                                            \
    return false;
#define NEVERD_KVM_X64_SPECIAL_DTABLE(Field)                                   \
  if (!sameState(L.Field, R.Field))                                            \
    return false;
#define NEVERD_KVM_X64_SPECIAL_VALUE(Field)                                    \
  if (L.Field != R.Field)                                                      \
    return false;
#define NEVERD_KVM_X64_SPECIAL_ARRAY(Field)                                    \
  if (!llvm::equal(L.Field, R.Field))                                          \
    return false;
#include "KvmX64State.def"
#undef NEVERD_KVM_X64_SPECIAL_ARRAY
#undef NEVERD_KVM_X64_SPECIAL_VALUE
#undef NEVERD_KVM_X64_SPECIAL_DTABLE
#undef NEVERD_KVM_X64_SPECIAL_SEGMENT
  return true;
}
bool sameGeneralRegisters(const X64MachineState &L, const X64MachineState &R) {
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  if (L.reg(X64Register::Name) != R.reg(X64Register::Name))                    \
    return false;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
  return true;
}
bool sameFPRegisters(const X64MachineState &L, const X64MachineState &R) {
  return L.FP == R.FP && L.MXCSR == R.MXCSR && L.Xmm == R.Xmm;
}
class KvmMachine final : public X64Machine, public KvmVM {
public:
  // Private startup execution discovers and verifies this mask before return.
  uint32_t MXCSRMask = x64::fp::ArchitecturalMXCSRMask;
  uint32_t mxcsrMask() const override { return MXCSRMask; }
  X64BranchModel BranchModel = X64BranchModel::Intel;
  X64BranchModel branchModel() const override { return BranchModel; }
  explicit KvmMachine(MemoryProjection &Memory) : Memory(Memory) {}
  void initializeSynchronizedRegisters() {
    const int Supported = ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_SYNC_REGS);
    if (Supported > 0)
      SynchronizedRegisters =
          Supported & (KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS);
    // Request capture only. Install changed inputs with SET_*REGS before
    // KVM_SET_GUEST_DEBUG, which associates single stepping with that RIP.
    Run->kvm_valid_regs = SynchronizedRegisters;
    Run->kvm_dirty_regs = 0;
  }
  bool requiresExceptionMonitor() const override { return true; }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
    // Only a completely captured debug exit proves the next entry runnable.
    // Failed, cancelled and exception entries must reestablish it explicitly.
    const bool WasRunnable = Runnable;
    Runnable = false;
    kvm_regs R{};
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  R.Field = State.reg(X64Register::Name);
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    kvm_sregs S{};
    const uint64_t Monitor = x64ExceptionMonitorBase(Memory);
    if (Monitor < x64::KernelMin ||
        !x64::canonicalRange(Monitor, x64::gateway::Bytes))
      return diagnostic::error(x64::exceptiontext::Gateway);
    // The ISA owns FP/SSE layout, TOP rotation and XSAVE init-state semantics.
    kvm_xsave F{};
    auto *Bytes = reinterpret_cast<uint8_t *>(F.region);
    const bool InstallFP =
        !WasRunnable || !sameFPRegisters(State, CapturedState);
    if (InstallFP)
      if (auto E = encodeX64XsaveState(State, {Bytes, sizeof(F.region)}, false,
                                       MXCSRMask))
        return E;
    auto Prepare = [&]() -> llvm::Error {
      if (WasRunnable && (SynchronizedRegisters & KVM_SYNC_X86_SREGS))
        S = CapturedSpecial;
      else if (ioctl(CPU, KVM_GET_SREGS, &S) < 0)
        return diagnostic::error(diagnostic::KvmState);
      const auto PreviousSpecial = S;
      S.cr0 = x64::CR0;
      S.cr3 = Root;
      S.cr4 = x64::CR4;
      S.efer = x64::EFER;
      S.cr8 = State.reg(X64Register::CR8);
      kvm_segment Code{}, Data{};
      Code.selector =
          State.UserMode ? x64::UserCodeSelector : x64::CodeSelector;
      Code.dpl = State.UserMode ? x64::UserPrivilege : 0;
      Code.type = x64::CodeType;
      Code.present = Code.s = Code.l = Code.g = 1;
      Code.limit = x64::SegmentLimit;
      Data.selector =
          State.UserMode ? x64::UserDataSelector : x64::DataSelector;
      Data.dpl = Code.dpl;
      Data.type = x64::DataType;
      Data.present = Data.s = Data.db = Data.g = 1;
      Data.limit = x64::SegmentLimit;
      S.cs = Code;
      S.ds = S.es = S.ss = S.fs = S.gs = Data;
      S.gs.base = State.GSBase;
      S.fs.base = State.FSBase;
      S.gdt.base = Monitor + x64::gateway::GDTOffset;
      S.gdt.limit = x64::gateway::GDTEntries * x64::WordBytes - 1;
      S.idt.base = Monitor + x64::gateway::IDTOffset;
      S.idt.limit = x64::gateway::VectorCount * x64::gateway::GateBytes - 1;
      S.tr = {};
      S.tr.selector = x64::gateway::TSSSelector;
      S.tr.base = Monitor + x64::gateway::TSSOffset;
      S.tr.limit = x64::gateway::TSSBytes - 1;
      S.tr.type = x64::gateway::TSSDescriptorType;
      S.tr.present = 1;
      kvm_mp_state MP{};
      MP.mp_state = KVM_MP_STATE_RUNNABLE;
      // Use actual special registers, from the last acknowledged run or an
      // explicit read. Reapply changed projection, CPL, TLS and CR8 fields.
      // Comparing protocol fields excludes kernel-owned structure padding.
      if ((!WasRunnable && ioctl(CPU, KVM_SET_MP_STATE, &MP) < 0) ||
          (!sameState(PreviousSpecial, S) &&
           ioctl(CPU, KVM_SET_SREGS, &S) < 0) ||
          ((!WasRunnable || !sameGeneralRegisters(State, CapturedState)) &&
           ioctl(CPU, KVM_SET_REGS, &R) < 0) ||
          (InstallFP && ioctl(CPU, KVM_SET_XSAVE, &F) < 0))
        return diagnostic::error(diagnostic::KvmState);
      // KVM associates software single stepping with the current linear RIP.
      // Arm it after any required register installation, including on resume.
      // A reused capture already describes the actual RIP at this boundary.
      kvm_guest_debug Debug{};
      Debug.control =
          KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_BLOCKIRQ;
      if (ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
        return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                       BackendAvailability::MissingCapability);
      // Without an in-kernel APIC, KVM_RUN takes CR8 from the shared run area,
      // even when KVM_SET_SREGS already installed that register.
      Run->cr8 = State.reg(X64Register::CR8);
      return llvm::Error::success();
    };
    auto Capture = [&]() -> llvm::Error {
      const bool Stepped = Run->exit_reason == KVM_EXIT_DEBUG &&
                           Run->debug.arch.exception == x64::DebugVector &&
                           (Run->debug.arch.dr6 & x64::DebugSingleStep);
      const bool Exception = Run->exit_reason == KVM_EXIT_HLT;
      // Synchronous exceptions enter a private IDT/IST and complete one HLT.
      // There is no unfinished KVM IO/MMIO operation to carry into a new entry.
      // Authenticate that gateway before publishing any architectural state.
      if (!Stepped && !Exception) {
        (void)ioctl(CPU, KVM_GET_REGS, &R);
        (void)ioctl(CPU, KVM_GET_SREGS, &S);
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            llvm::formatv(diagnostic::KvmExit, Run->exit_reason,
                          Run->exit_reason == KVM_EXIT_DEBUG
                              ? Run->debug.arch.exception
                          : Run->exit_reason == KVM_EXIT_FAIL_ENTRY
                              ? Run->fail_entry.hardware_entry_failure_reason
                              : 0,
                          R.rip, S.cr2)
                .str());
      }
      // KVM_RUN owns the shared packet until it returns. Capture it on the
      // same vCPU worker; gateway authentication precedes publication below.
      if (SynchronizedRegisters & KVM_SYNC_X86_REGS)
        R = Run->s.regs.regs;
      else if (ioctl(CPU, KVM_GET_REGS, &R) < 0)
        return diagnostic::error(diagnostic::KvmState);
      if (SynchronizedRegisters & KVM_SYNC_X86_SREGS)
        S = Run->s.regs.sregs;
      else if (Exception && ioctl(CPU, KVM_GET_SREGS, &S) < 0)
        return diagnostic::error(diagnostic::KvmState);
      if (ioctl(CPU, KVM_GET_XSAVE, &F) < 0)
        return diagnostic::error(diagnostic::KvmState);
      return llvm::Error::success();
    };
    auto Next = State;
    auto Complete = [&]() -> llvm::Error {
      if (auto E =
              decodeX64XsaveState(Next, {Bytes, sizeof(F.region)}, MXCSRMask))
        return E;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = R.Field;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
      if (Run->exit_reason == KVM_EXIT_HLT) {
        if (S.cs.selector != x64::CodeSelector)
          return diagnostic::error(x64::exceptiontext::Gateway);
        auto Trap = consumeX64ExceptionMonitor(Memory, Next, State, S.cr2);
        if (!Trap)
          return Trap.takeError();
        State = Next;
        return llvm::make_error<X64ExceptionError>(*Trap);
      }
      return llvm::Error::success();
    };
    if (auto E = runUntilExit(Control, Prepare, Capture, Complete))
      return E;
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::KvmRun, Control);
    State = Next;
    // Only acknowledged, fully decoded debug exits establish reusable state.
    // Host writes or context restoration are compared against this capture;
    // faults, failed transfers and cancellation invalidate it on next entry.
    CapturedState = Next;
    CapturedSpecial = S;
    Runnable = true;
    return llvm::Error::success();
  }

private:
  MemoryProjection &Memory;
  X64MachineState CapturedState;
  kvm_sregs CapturedSpecial{};
  uint64_t SynchronizedRegisters = 0;
  bool Runnable = false;
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createKvmMachine(MemoryProjection &Memory) {
  auto M = std::make_unique<KvmMachine>(Memory);
  if (auto E = M->initialize(Memory.registrations()))
    return E;
  if (ioctl(M->System, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) <= 0 ||
      ioctl(M->System, KVM_CHECK_EXTENSION, KVM_CAP_MP_STATE) <= 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
  kvm_guest_debug Debug{};
  Debug.control =
      KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_BLOCKIRQ;
  if (ioctl(M->CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
  M->initializeSynchronizedRegisters();
  if (auto E =
          verifyX64Machine(*M, Memory, &M->MXCSRMask, true, &M->BranchModel))
    return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>>
createKvmMachine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
