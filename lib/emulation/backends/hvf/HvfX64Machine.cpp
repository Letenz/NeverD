//===- HvfX64Machine.cpp - Native Intel Mac instruction transport --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64Exception.h"
#include "../../arch/x86_64/X64MachineProbe.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"

#if defined(__APPLE__) && defined(__x86_64__) && defined(NEVERD_EMULATION_HVF)
#include "HvfExecutor.h"

#include "llvm/Support/FormatVariadic.h"

#include <Hypervisor/hv_vmx.h>
#include <cpuid.h>

namespace neverd::emulation {
namespace {
// Intel SDM volume 3, VM-exit reasons and interruption-information format.
constexpr uint64_t ExitException = 0, ExitControlRegister = 28, ExitMTF = 37;
constexpr uint64_t ValidInterruption = 1ull << 31, ErrorCodeValid = 1 << 11;
constexpr uint64_t HardwareException = 3, SoftwareException = 6;

class IntelState {
public:
  explicit IntelState(hvf::Cpu CPU) : CPU(CPU) {}
  llvm::Error set(hv_x86_reg_t R, uint64_t V) {
    const auto S = hv_vcpu_write_register(CPU, R, V);
    return S ? hvf::error("hv_vcpu_write_register", S) : llvm::Error::success();
  }
  llvm::Expected<uint64_t> get(hv_x86_reg_t R) {
    uint64_t V = 0;
    if (auto S = hv_vcpu_read_register(CPU, R, &V))
      return hvf::error("hv_vcpu_read_register", S);
    return V;
  }
  llvm::Error vmcs(uint32_t Field, uint64_t V) {
    const auto S = hv_vmx_vcpu_write_vmcs(CPU, Field, V);
    if (S) {
      const auto Operation =
          llvm::formatv("hv_vmx_vcpu_write_vmcs(field={0:x}, value={1:x})",
                        Field, V)
              .str();
      return hvf::error(Operation.c_str(), S);
    }
    return llvm::Error::success();
  }
  llvm::Expected<uint64_t> vmcs(uint32_t Field) {
    uint64_t V = 0;
    if (auto S = hv_vmx_vcpu_read_vmcs(CPU, Field, &V))
      return hvf::error("hv_vmx_vcpu_read_vmcs", S);
    return V;
  }
  llvm::Error control(uint32_t Field, uint64_t Required,
                      uint64_t Forbidden = 0) {
    uint64_t Must = 0, May = 0;
    if (auto S = hv_vmx_vcpu_get_cap_write_vmcs(CPU, Field, &Must, &May))
      return hvf::unavailable("hv_vmx_vcpu_get_cap_write_vmcs", S);
    if ((Required & May) != Required || (Must & Forbidden))
      return diagnostic::unavailable("HVF lacks required Intel VMCS controls",
                                     BackendAvailability::MissingCapability);
    return vmcs(Field, Must | Required);
  }
  llvm::Error controlRegister(uint32_t Field, uint64_t Guest) {
    const bool IsCR4 = Field == VMCS_GUEST_CR4;
    uint64_t Must = 0, May = 0, FixedOne = 0, AllowedOne = 0;
    if (auto S = hv_vmx_vcpu_get_cap_write_vmcs(CPU, Field, &Must, &May))
      return hvf::unavailable("hv_vmx_vcpu_get_cap_write_vmcs", S);
    // The writable masks describe framework policy. VMX also imposes
    // hardware constraints (Intel SDM 24.8 / appendix A.7-A.8), including
    // CR4.VMXE. Those bits must not leak into the architectural guest view.
    if (auto S = hv_vmx_read_capability(
            IsCR4 ? HV_VMX_CAP_CR4_FIXED0 : HV_VMX_CAP_CR0_FIXED0, &FixedOne))
      return hvf::unavailable("hv_vmx_read_capability(FIXED0)", S);
    if (auto S = hv_vmx_read_capability(
            IsCR4 ? HV_VMX_CAP_CR4_FIXED1 : HV_VMX_CAP_CR0_FIXED1, &AllowedOne))
      return hvf::unavailable("hv_vmx_read_capability(FIXED1)", S);
    const uint64_t Native = Guest | Must | FixedOne;
    if (Native & ~(May & AllowedOne))
      return diagnostic::unavailable(
          "HVF lacks required Intel control-register state",
          BackendAvailability::MissingCapability);
    if (auto E = control(IsCR4 ? VMCS_CTRL_CR4_MASK : VMCS_CTRL_CR0_MASK,
                         Native ^ Guest))
      return E;
    if (auto E =
            vmcs(IsCR4 ? VMCS_CTRL_CR4_SHADOW : VMCS_CTRL_CR0_SHADOW, Guest))
      return E;
    return vmcs(Field, Native);
  }
  llvm::Error prepare(const X64MachineState &State, uint64_t Root,
                      llvm::MutableArrayRef<uint8_t> FP) {
    // MTF leaves architectural RFLAGS untouched. The framework owns EPT and
    // its host VMCS fields; negotiate the fields it permits clients to write.
    if (auto E = control(VMCS_CTRL_PIN_BASED, PIN_BASED_INTR | PIN_BASED_NMI))
      return E;
    if (auto E =
            control(VMCS_CTRL_CPU_BASED,
                    CPU_BASED_MTF | CPU_BASED_CR8_LOAD | CPU_BASED_CR8_STORE |
                        CPU_BASED_SECONDARY_CTLS | CPU_BASED_HLT,
                    CPU_BASED_TPR_SHADOW))
      return E;
    if (auto E = control(VMCS_CTRL_CPU_BASED2, CPU_BASED2_EPT))
      return E;
    if (auto E = control(VMCS_CTRL_VMENTRY_CONTROLS,
                         VMENTRY_GUEST_IA32E | VMENTRY_LOAD_EFER))
      return E;
    // VM-exit controls restore the framework's host state. Retain the
    // configuration established by hv_vcpu_create instead of deriving it
    // from writable-bit masks for a guest packet.
    if (auto E = controlRegister(VMCS_GUEST_CR0, x64::CR0))
      return E;
    if (auto E = controlRegister(VMCS_GUEST_CR4, x64::CR4 | x64::fp::OSXsave))
      return E;
    // The framework owns the VMCS link pointer; it is not a writable guest
    // field. Its initialization belongs to hv_vcpu_create, not this packet.
    const std::pair<uint32_t, uint64_t> Fields[] = {
        {VMCS_CTRL_EXC_BITMAP, x64::ExceptionExitBitmap},
        {VMCS_CTRL_PF_ERROR_MASK, 0},
        {VMCS_CTRL_PF_ERROR_MATCH, 0},
        {VMCS_CTRL_VMENTRY_IRQ_INFO, 0},
        {VMCS_CTRL_CR3_COUNT, 0},
        {VMCS_CTRL_TPR_THRESHOLD, 0},
        {VMCS_GUEST_ACTIVITY_STATE, 0},
        {VMCS_GUEST_INTERRUPTIBILITY, 0},
        {VMCS_GUEST_DEBUG_EXC, 0},
        {VMCS_GUEST_DR7, 0x400},
        {VMCS_GUEST_IA32_EFER, x64::EFER},
        {VMCS_GUEST_CR3, Root},
        {VMCS_GUEST_GDTR_BASE, 0},
        {VMCS_GUEST_GDTR_LIMIT, 0},
        {VMCS_GUEST_IDTR_BASE, 0},
        {VMCS_GUEST_IDTR_LIMIT, 0},
        {VMCS_GUEST_LDTR, 0},
        {VMCS_GUEST_LDTR_BASE, 0},
        {VMCS_GUEST_LDTR_LIMIT, 0},
        {VMCS_GUEST_LDTR_AR, 1 << 16},
        {VMCS_GUEST_TR, 0x28},
        {VMCS_GUEST_TR_BASE, 0},
        {VMCS_GUEST_TR_LIMIT, 0x67},
        {VMCS_GUEST_TR_AR, 0x8b}};
    for (auto [Field, V] : Fields)
      if (auto E = vmcs(Field, V))
        return E;
    // Segment field offsets are architectural VMCS encodings from the SDK.
    for (unsigned N = 0; N < 6; ++N) {
      const bool Code = N == 1;
      const auto Selector =
          State.UserMode
              ? (Code ? x64::UserCodeSelector : x64::UserDataSelector)
              : (Code ? x64::CodeSelector : x64::DataSelector);
      const uint64_t Access =
          (Code ? 0xa09b : 0xc093) | (State.UserMode ? 3 << 5 : 0);
      const uint64_t Base = N == 4 ? State.FSBase : N == 5 ? State.GSBase : 0;
      for (auto [Field, V] :
           {std::pair<uint32_t, uint64_t>{VMCS_GUEST_ES + 2 * N, Selector},
            {VMCS_GUEST_ES_BASE + 2 * N, Base},
            {VMCS_GUEST_ES_LIMIT + 2 * N, x64::SegmentLimit},
            {VMCS_GUEST_ES_AR + 2 * N, Access}})
        if (auto E = vmcs(Field, V))
          return E;
    }
#define NEVERD_HVF_X64_REGISTER(Name, Native)                                  \
  if (auto E = set(HV_X86_##Native, State.reg(X64Register::Name)))             \
    return E;
#include "HvfX64Registers.def"
#undef NEVERD_HVF_X64_REGISTER
    if (auto E = set(HV_X86_XCR0, x64::fp::FPAndSSE))
      return E;
    if (auto E = encodeX64XsaveState(State, FP))
      return E;
    if (auto S = hv_vcpu_write_fpstate(CPU, FP.data(), FP.size()))
      return hvf::error("hv_vcpu_write_fpstate", S);
    if (auto S = hv_vcpu_invalidate_tlb(CPU))
      return hvf::error("hv_vcpu_invalidate_tlb", S);
    return llvm::Error::success();
  }
  llvm::Error capture(X64MachineState &State,
                      llvm::MutableArrayRef<uint8_t> FP) {
#define NEVERD_HVF_X64_REGISTER(Name, Native)                                  \
  {                                                                            \
    auto V = get(HV_X86_##Native);                                             \
    if (!V)                                                                    \
      return V.takeError();                                                    \
    State.reg(X64Register::Name) = *V;                                         \
  }
#include "HvfX64Registers.def"
#undef NEVERD_HVF_X64_REGISTER
    // CR8 is an architectural shadow. HV_X86_TPR readback can disagree with
    // guest MOV CR8 on Intel hosts, so all CR8 accesses must exit. Checked
    // code cannot write CR8; an authenticated read is completed below.
    auto FS = vmcs(VMCS_GUEST_FS_BASE);
    if (!FS)
      return FS.takeError();
    auto GS = vmcs(VMCS_GUEST_GS_BASE);
    if (!GS)
      return GS.takeError();
    State.FSBase = *FS;
    State.GSBase = *GS;
    std::fill(FP.begin(), FP.end(), 0);
    if (auto S = hv_vcpu_read_fpstate(CPU, FP.data(), FP.size()))
      return hvf::error("hv_vcpu_read_fpstate", S);
    return decodeX64XsaveState(State, FP);
  }

private:
  hvf::Cpu CPU;
};
class HvfX64Machine final : public X64Machine {
public:
  HvfX64Machine(std::shared_ptr<hvf::Executor> Executor,
                MemoryProjection &Memory, unsigned FPBytes)
      : Host(std::move(Executor), Memory), FPBytes(FPBytes) {}
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    auto Next = State;
    auto E = Host.execute(Control, [&](hvf::Executor &Executor) -> llvm::Error {
      Control = Control.forNativeStep();
      IntelState Native(Executor.cpu());
      llvm::MutableArrayRef<uint8_t> Bytes(FP.data(), FPBytes);
      if (auto E = Native.prepare(State, Root, Bytes))
        return E;
      return Executor.run(Control, [&](bool Cancelled) -> llvm::Error {
        auto Reason = Native.vmcs(VMCS_RO_EXIT_REASON);
        if (!Reason)
          return Reason.takeError();
        // External-interrupt exits can carry an acknowledged host kick.
        if (Cancelled && *Reason == 1)
          return llvm::Error::success();
        if (*Reason != ExitMTF && *Reason != ExitException &&
            *Reason != ExitControlRegister)
          return diagnostic::error("HVF Intel unexpected VM exit");
        if (auto E = Native.capture(Next, Bytes))
          return E;
        if (*Reason == ExitControlRegister) {
          auto Qualification = Native.vmcs(VMCS_RO_EXIT_QUALIFIC);
          if (!Qualification)
            return Qualification.takeError();
          // Intel SDM: bits 3:0 select CR8, 5:4 select MOV from CR, and
          // 11:8 select the GPR. Reject every other control-register exit.
          if ((*Qualification & ~uint64_t(0xf00)) != 0x18)
            return diagnostic::error(
                "HVF Intel unsupported control-register exit");
          auto Length = Native.vmcs(VMCS_RO_VMEXIT_INSTR_LEN);
          if (!Length)
            return Length.takeError();
          if (auto E = completeX64CR8Read(Next, (*Qualification >> 8) & 0xf,
                                          *Length))
            return E;
        }
        if (*Reason == ExitException) {
          auto Info = Native.vmcs(VMCS_RO_VMEXIT_IRQ_INFO);
          if (!Info)
            return Info.takeError();
          const auto Vector = unsigned(*Info & 0xff);
          const auto Type = (*Info >> 8) & 7;
          if (!(*Info & ValidInterruption) ||
              (Type != HardwareException && Type != SoftwareException) ||
              !x64::isExceptionVector(Vector) ||
              !(x64::ExceptionExitBitmap & (1ull << Vector)) ||
              bool(*Info & ErrorCodeValid) != x64::exceptionHasError(Vector))
            return diagnostic::error("HVF Intel invalid exception exit");
          if (Type == SoftwareException && x64::exceptionIsTrap(Vector)) {
            auto Length = Native.vmcs(VMCS_RO_VMEXIT_INSTR_LEN);
            if (!Length)
              return Length.takeError();
            if (!*Length || *Length > x64::MaxInstructionBytes)
              return diagnostic::error(
                  "HVF Intel invalid exception instruction length");
            Next.reg(X64Register::PC) += *Length;
          }
          X64Exception Fault{Vector, {}, {}};
          if (*Info & ErrorCodeValid) {
            auto Error = Native.vmcs(VMCS_RO_VMEXIT_IRQ_ERROR);
            if (!Error)
              return Error.takeError();
            Fault.ErrorCode = *Error;
          }
          if (Vector == unsigned(x64::ExceptionVector::PageFault)) {
            auto Address = Native.vmcs(VMCS_RO_EXIT_QUALIFIC);
            if (!Address)
              return Address.takeError();
            Fault.FaultAddress = *Address;
          }
          Next.reg(X64Register::FLAGS) =
              (Next.reg(X64Register::FLAGS) & ~x64::ResumeFlag) |
              (State.reg(X64Register::FLAGS) & x64::ResumeFlag);
          return llvm::make_error<X64ExceptionError>(Fault);
        }
        if (Control.interrupted())
          return diagnostic::interrupted("HVF Intel capture interrupted",
                                         Control);
        return llvm::Error::success();
      });
    });
    if (E) {
      if (E.isA<X64ExceptionError>())
        State = Next;
      return E;
    }
    State = Next;
    return llvm::Error::success();
  }

private:
  hvf::Binding Host;
  unsigned FPBytes;
  alignas(64) std::array<uint8_t, x64::fp::MaxXsaveBytes> FP{};
};
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createHvfX64Machine(MemoryProjection &Memory) {
  auto Host = hvf::Executor::acquire();
  if (!Host)
    return Host.takeError();
  unsigned A, B, C, D;
  if (!__get_cpuid(1, &A, &B, &C, &D) || !(C & bit_OSXSAVE) ||
      !__get_cpuid_count(0xd, 0, &A, &B, &C, &D) || B < x64::fp::XsaveBytes ||
      B > x64::fp::MaxXsaveBytes)
    return diagnostic::unavailable("HVF requires host XSAVE support",
                                   BackendAvailability::MissingCapability);
  auto Machine = std::make_unique<HvfX64Machine>(std::move(*Host), Memory, B);
  if (auto E = verifyX64Machine(*Machine, Memory))
    return E;
  return std::unique_ptr<X64Machine>(std::move(Machine));
}
} // namespace neverd::emulation
#else
namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>>
createHvfX64Machine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
