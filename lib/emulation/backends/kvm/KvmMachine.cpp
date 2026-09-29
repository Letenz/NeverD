//===- KvmMachine.cpp - Linux x64 single-step execution ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
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
#define NEVERD_KVM_X64_STATE(Name, Value) constexpr uint64_t Name = Value;
#include "KvmX64State.def"
#undef NEVERD_KVM_X64_STATE
class KvmMachine final : public X64Machine, public KvmVM {
public:
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
    // x87 is unobservable in this profile and has a fixed reset value. Every
    // admitted XMM and MXCSR bit belongs to the architecture-owned State.
    // SET_FPU does not establish XSTATE_BV or transfer MXCSR on x86. Use the
    // standard XSAVE ABI so XRSTOR retains seeded XMM values on the first run.
    kvm_xsave F{};
    auto *Bytes = reinterpret_cast<uint8_t *>(F.region);
    llvm::support::endian::write16le(Bytes + FCWOffset, x64::InitialFCW);
    llvm::support::endian::write32le(Bytes + MXCSROffset, State.MXCSR);
    llvm::support::endian::write64le(Bytes + XStateBVOffset, FPAndSSE);
    std::memcpy(Bytes + XmmOffset, State.Xmm.data(), sizeof(State.Xmm));
    if (ioctl(CPU, KVM_SET_SREGS, &S) < 0 || ioctl(CPU, KVM_SET_REGS, &R) < 0 ||
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
    // There is no generic KVM userspace exception bitmap. An unexpected exit
    // is terminal; this checked profile admits no instruction that should
    // require exception delivery or an unfinished IO/MMIO completion.
    if (Run->exit_reason != KVM_EXIT_DEBUG ||
        Run->debug.arch.exception != x64::DebugVector ||
        !(Run->debug.arch.dr6 & x64::DebugSingleStep)) {
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
    if (ioctl(CPU, KVM_GET_REGS, &R) < 0 || ioctl(CPU, KVM_GET_XSAVE, &F) < 0)
      return diagnostic::error(diagnostic::KvmState);
    std::memcpy(State.Xmm.data(), Bytes + XmmOffset, sizeof(State.Xmm));
    State.MXCSR = llvm::support::endian::read32le(Bytes + MXCSROffset);
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  State.reg(X64Register::Name) = R.Field;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
    return llvm::Error::success();
  }
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createKvmMachine(MemoryProjection &Memory) {
  auto M = std::make_unique<KvmMachine>();
  if (auto E = M->initialize(Memory.registrations()))
    return E;
  if (ioctl(M->System, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) <= 0)
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
