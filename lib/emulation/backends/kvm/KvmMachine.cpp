//===- KvmMachine.cpp - Linux x64 single-step execution ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64ExceptionMonitor.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../MachineFactories.h"

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
class KvmMachine final : public X64Machine, public KvmVM {
public:
  explicit KvmMachine(MemoryProjection &Memory) : Memory(Memory) {}
  bool requiresExceptionMonitor() const override { return true; }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    kvm_regs R{};
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  R.Field = State.reg(X64Register::Name);
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    kvm_sregs S{};
    if (ioctl(CPU, KVM_GET_SREGS, &S) < 0)
      return diagnostic::error(diagnostic::KvmState);
    S.cr0 = x64::CR0;
    S.cr3 = Root;
    S.cr4 = x64::CR4;
    S.efer = x64::EFER;
    S.cr8 = State.reg(X64Register::CR8);
    kvm_segment Code{}, Data{};
    Code.selector = State.UserMode ? x64::UserCodeSelector : x64::CodeSelector;
    Code.dpl = State.UserMode ? x64::UserPrivilege : 0;
    Code.type = x64::CodeType;
    Code.present = Code.s = Code.l = Code.g = 1;
    Code.limit = x64::SegmentLimit;
    Data.selector = State.UserMode ? x64::UserDataSelector : x64::DataSelector;
    Data.dpl = Code.dpl;
    Data.type = x64::DataType;
    Data.present = Data.s = Data.db = Data.g = 1;
    Data.limit = x64::SegmentLimit;
    S.cs = Code;
    S.ds = S.es = S.ss = S.fs = S.gs = Data;
    S.gs.base = State.GSBase;
    S.fs.base = State.FSBase;
    const uint64_t Monitor = x64ExceptionMonitorBase(Memory);
    if (Monitor < x64::KernelMin ||
        !x64::canonicalRange(Monitor, x64::gateway::Bytes))
      return diagnostic::error(x64::exceptiontext::Gateway);
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
    // The ISA owns the legacy FP/SSE layout; KVM owns the XSAVE header.
    kvm_xsave F{};
    auto *Bytes = reinterpret_cast<uint8_t *>(F.region);
    if (auto E = encodeX64FXState(State, {Bytes, sizeof(F.region)}))
      return E;
    llvm::support::endian::write64le(Bytes + x64::fp::XStateOffset,
                                     x64::fp::FPAndSSE);
    kvm_mp_state MP{};
    MP.mp_state = KVM_MP_STATE_RUNNABLE;
    if (ioctl(CPU, KVM_SET_MP_STATE, &MP) < 0 ||
        ioctl(CPU, KVM_SET_SREGS, &S) < 0 || ioctl(CPU, KVM_SET_REGS, &R) < 0 ||
        ioctl(CPU, KVM_SET_XSAVE, &F) < 0)
      return diagnostic::error(diagnostic::KvmState);
    // KVM associates software single stepping with the current linear RIP.
    // Arm it after installing this invocation's registers, including on resume.
    kvm_guest_debug Debug{};
    Debug.control =
        KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_BLOCKIRQ;
    if (ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::MissingCapability);
    if (auto E = runUntilExit(Control.forNativeStep()))
      return E;
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
    if (ioctl(CPU, KVM_GET_REGS, &R) < 0 || ioctl(CPU, KVM_GET_XSAVE, &F) < 0 ||
        (Exception && ioctl(CPU, KVM_GET_SREGS, &S) < 0))
      return diagnostic::error(diagnostic::KvmState);
    auto Next = State;
    if (auto E = decodeX64FXState(Next, {Bytes, sizeof(F.region)}))
      return E;
    const auto Present =
        llvm::support::endian::read64le(Bytes + x64::fp::XStateOffset);
    if (!(Present & x64::fp::X87Present))
      Next.FP = {};
    if (!(Present & x64::fp::SSEPresent)) {
      Next.MXCSR = x64::InitialMXCSR;
      Next.Xmm = {};
    }
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = R.Field;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    if (Exception) {
      if (S.cs.selector != x64::CodeSelector)
        return diagnostic::error(x64::exceptiontext::Gateway);
      auto Trap = consumeX64ExceptionMonitor(Memory, Next, State, S.cr2);
      if (!Trap)
        return Trap.takeError();
      State = Next;
      return llvm::make_error<X64ExceptionError>(*Trap);
    }
    State = Next;
    return llvm::Error::success();
  }

private:
  MemoryProjection &Memory;
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
