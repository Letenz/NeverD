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
#include <vector>

namespace neverd::emulation {
namespace {
constexpr uint32_t FSBaseMSR = 0xc0000100, KernelGSBaseMSR = 0xc0000102;
llvm::Expected<uint64_t> kernelGSBase(int CPU) {
  struct {
    uint32_t Count = 1, Padding = 0;
    kvm_msr_entry Entry{};
  } Value;
  static_assert(offsetof(decltype(Value), Entry) == sizeof(kvm_msrs));
  Value.Entry.index = KernelGSBaseMSR;
  if (ioctl(CPU, KVM_GET_MSRS, &Value) != 1)
    return diagnostic::error(diagnostic::KvmState);
  return Value.Entry.data;
}
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
  llvm::Error configureDirectMSRFilter() {
#if defined(KVM_CAP_X86_MSR_FILTER) && defined(KVM_CAP_X86_USER_SPACE_MSR)
    if (ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_X86_MSR_FILTER) <= 0 ||
        ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_X86_USER_SPACE_MSR) <= 0 ||
        ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_DEBUGREGS) <= 0 ||
        ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_READONLY_MEM) <= 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::MissingCapability);
    kvm_enable_cap Capability{};
    Capability.cap = KVM_CAP_X86_USER_SPACE_MSR;
    Capability.args[0] = KVM_MSR_EXIT_REASON_FILTER;
    if (ioctl(VM, KVM_ENABLE_CAP, &Capability) < 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::MissingCapability);
    // KVM owns reads in its ordinary architectural/vendor MSR windows. The
    // default-deny rule intercepts every write except the two TLS bases that
    // our public context retains. It also refuses other read windows without
    // supplying invented register values or completing an unhandled exit.
    std::array<uint8_t, KVM_MSR_FILTER_MAX_BITMAP_SIZE> Readable;
    Readable.fill(0xff);
    uint8_t TLS = 3;
    kvm_msr_filter Filter{};
    Filter.flags = KVM_MSR_FILTER_DEFAULT_DENY;
    unsigned Index = 0;
    for (uint32_t Base : {0u, 0x40000000u, 0xc0000000u, 0xc0010000u})
      Filter.ranges[Index++] = {KVM_MSR_FILTER_READ,
                                uint32_t(Readable.size() * 8), Base,
                                Readable.data()};
    Filter.ranges[Index] = {KVM_MSR_FILTER_WRITE, 2, FSBaseMSR, &TLS};
    if (ioctl(VM, KVM_X86_SET_MSR_FILTER, &Filter) < 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::MissingCapability);
    return llvm::Error::success();
#else
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
#endif
  }
  llvm::Error enableDirectSupervisor() {
#if defined(KVM_CAP_X86_MSR_FILTER) && defined(KVM_CAP_X86_USER_SPACE_MSR)
    const int Slots = ioctl(System, KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS);
    if (Slots <= 0)
      return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                     BackendAvailability::MissingCapability);
    MaxMemorySlots = unsigned(Slots);
    for (const auto &Mapping : Memory.registrations()) {
      kvm_userspace_memory_region Region{};
      Region.slot = NativeSlots.size();
      Region.guest_phys_addr = Mapping.Physical;
      Region.memory_size = Mapping.Size;
      Region.userspace_addr = reinterpret_cast<uintptr_t>(Mapping.Backing);
      NativeSlots.push_back(Region);
    }
    DirectSupervisor = true;
    return llvm::Error::success();
#else
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
#endif
  }
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
  llvm::Error
  withExecutionOwner(llvm::function_ref<llvm::Error()> Loop) override {
    return KvmVM::withExecutionOwner(Loop);
  }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    return enter(State, Root, Control.forNativeStep(), true);
  }
  llvm::Error run(X64MachineState &State, uint64_t Root,
                  MachineRunControl Control) override {
    return enter(State, Root, Control, false);
  }
  bool supportsExecutionStops() const override { return true; }
  llvm::Error runTo(X64MachineState &State, uint64_t Root,
                    MachineRunControl Control,
                    llvm::ArrayRef<uint64_t> Stops) override {
    return enter(State, Root, Control, false, Stops);
  }

private:
  llvm::Error protectPhysicalWrites() {
    std::vector<MemoryWriteWatch> Guards;
    for (const auto &W : Memory.projectionWriteGuards()) {
      const uint64_t First = W.Address & ~(x64::PageSize - 1);
      const uint64_t End = (W.Address + W.Size - 1) | (x64::PageSize - 1);
      Guards.push_back({First, End - First + 1});
    }
    llvm::sort(Guards, [](const auto &L, const auto &R) {
      return L.Address < R.Address;
    });
    std::vector<MemoryWriteWatch> Merged;
    for (const auto &G : Guards) {
      if (!Merged.empty() &&
          G.Address <= Merged.back().Address + Merged.back().Size) {
        auto &Last = Merged.back();
        Last.Size = std::max(Last.Address + Last.Size, G.Address + G.Size) -
                    Last.Address;
      } else {
        Merged.push_back(G);
      }
    }
    std::vector<kvm_userspace_memory_region> Next;
    for (const auto &Mapping : Memory.registrations()) {
      const uint64_t Begin = Memory.transportOffset(Mapping.Physical);
      const uint64_t End = Begin + Mapping.Size;
      uint64_t At = Begin;
      const auto Append = [&](uint64_t Last, bool ReadOnly) {
        if (Last == At)
          return;
        kvm_userspace_memory_region Region{};
        Region.slot = Next.size();
        Region.flags = ReadOnly ? KVM_MEM_READONLY : 0;
        Region.guest_phys_addr = Memory.transportPhysical(At);
        Region.memory_size = Last - At;
        Region.userspace_addr =
            reinterpret_cast<uintptr_t>(Mapping.Backing) + At - Begin;
        Next.push_back(Region);
        At = Last;
      };
      for (const auto &G : Merged) {
        const uint64_t First = std::max(Begin, G.Address);
        const uint64_t Last = std::min(End, G.Address + G.Size);
        if (First >= Last)
          continue;
        Append(First, false);
        Append(Last, true);
      }
      Append(End, false);
    }
    if (Next.size() > MaxMemorySlots)
      return diagnostic::error(diagnostic::KvmDirectSlots);
    const bool SameLayout =
        llvm::equal(NativeSlots, Next, [](const auto &L, const auto &R) {
          return L.slot == R.slot && L.guest_phys_addr == R.guest_phys_addr &&
                 L.memory_size == R.memory_size &&
                 L.userspace_addr == R.userspace_addr;
        });
    if (!SameLayout) {
      // KVM permits neither resizing a slot nor changing its READONLY flag.
      // The vCPU is stopped; remove the old layout before registering its
      // split.
      for (auto Region : NativeSlots) {
        Region.memory_size = 0;
        if (ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Region) < 0)
          return diagnostic::error(diagnostic::KvmMap);
      }
    }
    for (size_t I = 0; I < Next.size(); ++I) {
      if (SameLayout && NativeSlots[I].flags == Next[I].flags)
        continue;
      if (SameLayout) {
        auto Previous = NativeSlots[I];
        Previous.memory_size = 0;
        if (ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Previous) < 0)
          return diagnostic::error(diagnostic::KvmMap);
      }
      if (ioctl(VM, KVM_SET_USER_MEMORY_REGION, &Next[I]) < 0)
        return diagnostic::error(diagnostic::KvmMap);
    }
    NativeSlots = std::move(Next);
    return llvm::Error::success();
  }
  bool physicalWriteProtected(uint64_t Address) const {
    return llvm::any_of(NativeSlots, [&](const auto &R) {
      return (R.flags & KVM_MEM_READONLY) && Address >= R.guest_phys_addr &&
             Address - R.guest_phys_addr < R.memory_size;
    });
  }
  /// One hardware entry. A single step ends at the next instruction boundary;
  /// a free entry ends only at a synchronous exception or an interruption.
  llvm::Error enter(X64MachineState &State, uint64_t Root,
                    MachineRunControl Control, bool Single,
                    llvm::ArrayRef<uint64_t> Stops = {}) {
    if (DirectSupervisor && (State.UserMode || Poisoned))
      return diagnostic::error(diagnostic::KvmDirectState);
    if (Stops.size() > x64::ExecutionStopCount ||
        llvm::any_of(Stops, [](uint64_t PC) { return !x64::canonical(PC); }))
      return diagnostic::error(diagnostic::DirectExecutionUnsupported);
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
    kvm_sregs EntrySpecial{};
    kvm_debugregs EntryDebug{};
    uint64_t EntryKernelGS = 0;
    std::vector<uint8_t> EntryMonitor;
    bool Prepared = false, Captured = false, UnsupportedMSR = false;
    bool UnsupportedWrite = false;
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
      if (DirectSupervisor)
        if (auto E = protectPhysicalWrites())
          return E;
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
      // A free entry disarms it: the guest then owns its own trap flag, and
      // its debug exceptions arrive through the private gateway like others.
      kvm_guest_debug Debug{};
      if (Single)
        Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP |
                        KVM_GUESTDBG_BLOCKIRQ;
      else if (!Stops.empty()) {
        Debug.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
        for (unsigned I = 0; I < Stops.size(); ++I)
          Debug.arch.debugreg[I] = Stops[I];
        Debug.arch.debugreg[7] = x64::executionStopControl(Stops.size());
      }
      if (ioctl(CPU, KVM_SET_GUEST_DEBUG, &Debug) < 0)
        return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                       BackendAvailability::MissingCapability);
      // Without an in-kernel APIC, KVM_RUN takes CR8 from the shared run area,
      // even when KVM_SET_SREGS already installed that register.
      Run->cr8 = State.reg(X64Register::CR8);
      if (DirectSupervisor) {
        EntrySpecial = S;
        if (ioctl(CPU, KVM_GET_DEBUGREGS, &EntryDebug) < 0)
          return diagnostic::error(diagnostic::KvmState);
        auto KernelGS = kernelGSBase(CPU);
        if (!KernelGS)
          return KernelGS.takeError();
        EntryKernelGS = *KernelGS;
        EntryMonitor.resize(2 * x64::PageSize);
        std::memcpy(EntryMonitor.data(), Memory.data() + x64::gateway::DataGPA,
                    EntryMonitor.size());
      }
      Prepared = true;
      return llvm::Error::success();
    };
    auto Capture = [&]() -> llvm::Error {
      const bool Stepped = Single && Run->exit_reason == KVM_EXIT_DEBUG &&
                           Run->debug.arch.exception == x64::DebugVector &&
                           (Run->debug.arch.dr6 & x64::DebugSingleStep);
      const bool Exception = Run->exit_reason == KVM_EXIT_HLT;
      const bool Debugged = !Single && !Stops.empty() &&
                            Run->exit_reason == KVM_EXIT_DEBUG &&
                            Run->debug.arch.exception == x64::DebugVector;
#if defined(KVM_EXIT_X86_RDMSR) && defined(KVM_EXIT_X86_WRMSR)
      UnsupportedMSR =
          DirectSupervisor && (Run->exit_reason == KVM_EXIT_X86_RDMSR ||
                               Run->exit_reason == KVM_EXIT_X86_WRMSR);
#endif
      UnsupportedWrite =
          DirectSupervisor && Run->exit_reason == KVM_EXIT_MMIO &&
          Run->mmio.is_write && physicalWriteProtected(Run->mmio.phys_addr);
      // Synchronous exceptions enter a private IDT/IST and complete one HLT.
      // There is no unfinished KVM IO/MMIO operation to carry into a new entry.
      // Authenticate that gateway before publishing any architectural state.
      if (!Stepped && !Exception && !Debugged && !UnsupportedMSR &&
          !UnsupportedWrite) {
        (void)ioctl(CPU, KVM_GET_REGS, &R);
        (void)ioctl(CPU, KVM_GET_SREGS, &S);
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            llvm::formatv(diagnostic::KvmExit, Run->exit_reason,
                          Run->exit_reason == KVM_EXIT_DEBUG
                              ? Run->debug.arch.exception
                          : Run->exit_reason == KVM_EXIT_FAIL_ENTRY
                              ? Run->fail_entry.hardware_entry_failure_reason
                          : Run->exit_reason == KVM_EXIT_INTERNAL_ERROR
                              ? Run->internal.suberror
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
      else if ((Exception || DirectSupervisor) &&
               ioctl(CPU, KVM_GET_SREGS, &S) < 0)
        return diagnostic::error(diagnostic::KvmState);
      if (ioctl(CPU, KVM_GET_XSAVE, &F) < 0)
        return diagnostic::error(diagnostic::KvmState);
      Captured = true;
      return llvm::Error::success();
    };
    auto Next = State;
    const auto ValidateSpecial = [&](const X64Exception *Trap,
                                     bool DebugBoundary) -> llvm::Error {
      if (!DirectSupervisor)
        return llvm::Error::success();
      const auto Refuse = [&]() {
        Poisoned = true;
        State = Next;
        return llvm::make_error<X64UnsupportedStateError>(
            diagnostic::KvmDirectState);
      };
      Next.GSBase = S.gs.base;
      Next.FSBase = S.fs.base;
      Next.reg(X64Register::CR8) = S.cr8;
      auto Normalized = S;
      // The public context represents these fields. All other live special
      // state must match the entry; do not erase a guest change on re-entry.
      Normalized.cr8 = EntrySpecial.cr8;
      Normalized.fs.base = EntrySpecial.fs.base;
      Normalized.gs.base = EntrySpecial.gs.base;
      if (Trap) {
        // Only an authenticated monitor frame authorizes normalization of
        // the gateway's CS/SS and a processor-written page-fault address.
        Normalized.cs = EntrySpecial.cs;
        Normalized.ss = EntrySpecial.ss;
        if (Trap->Vector == unsigned(x64::ExceptionVector::PageFault))
          Normalized.cr2 = EntrySpecial.cr2;
      }
      if (!sameState(Normalized, EntrySpecial) ||
          !std::equal(EntryMonitor.begin(), EntryMonitor.end(),
                      Memory.data() + x64::gateway::DataGPA))
        return Refuse();
      kvm_debugregs Debug{};
      if (ioctl(CPU, KVM_GET_DEBUGREGS, &Debug) < 0)
        return diagnostic::error(diagnostic::KvmState);
      auto KernelGS = kernelGSBase(CPU);
      if (!KernelGS)
        return KernelGS.takeError();
      if (!llvm::equal(Debug.db, EntryDebug.db) ||
          Debug.dr7 != EntryDebug.dr7 || Debug.flags != EntryDebug.flags ||
          *KernelGS != EntryKernelGS)
        return Refuse();
      // Ignore only status bits produced by our private step/watch. A guest
      // debug exception may itself change state the public context cannot
      // retain, in which case this conservative profile must stop.
      const uint64_t Instrumentation =
          DebugBoundary ? x64::DebugSingleStep | 0xf : 0;
      if ((Debug.dr6 ^ EntryDebug.dr6) & ~Instrumentation)
        return Refuse();
      return llvm::Error::success();
    };
    auto Complete = [&]() -> llvm::Error {
      if (UnsupportedWrite) {
        // MMIO has a pending kernel operation. Its complete architectural
        // state is unavailable until reentry, which would finish the rejected
        // write. Keep this a terminal diagnostic; publish no partial context.
        Poisoned = true;
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            llvm::formatv("{0} (KVM RIP=0x{1:x}, physical=0x{2:x})",
                          diagnostic::KvmDirectMemory, R.rip,
                          Run->mmio.phys_addr)
                .str());
      }
      if (auto E =
              decodeX64XsaveState(Next, {Bytes, sizeof(F.region)}, MXCSRMask))
        return E;
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = R.Field;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
      if (UnsupportedMSR) {
        if (auto E = ValidateSpecial(nullptr, false))
          return E;
        State = Next;
        Poisoned = true;
        return llvm::make_error<X64UnsupportedStateError>(
            diagnostic::KvmDirectMSR);
      }
      if (Run->exit_reason == KVM_EXIT_HLT) {
        if (S.cs.selector != x64::CodeSelector)
          return diagnostic::error(x64::exceptiontext::Gateway);
        auto Trap =
            consumeX64ExceptionMonitor(Memory, Next, State, S.cr2, Single);
        if (!Trap)
          return Trap.takeError();
        if (auto E = ValidateSpecial(&*Trap, false))
          return E;
        State = Next;
        return llvm::make_error<X64ExceptionError>(*Trap);
      }
      if (!Single && Run->exit_reason == KVM_EXIT_DEBUG) {
        if (!x64::executionStopHit(Stops, R.rip, Run->debug.arch.dr6)) {
          X64Exception Trap{x64::DebugVector, std::nullopt, std::nullopt};
          if (auto E = ValidateSpecial(nullptr, true))
            return E;
          State = Next;
          return llvm::make_error<X64ExceptionError>(Trap);
        }
        Next.reg(X64Register::FLAGS) &= ~x64::ResumeFlag;
      }
      if (auto E = ValidateSpecial(nullptr, Run->exit_reason == KVM_EXIT_DEBUG))
        return E;
      return llvm::Error::success();
    };
    if (auto E = runUntilExit(Control, Prepare, Capture, Complete)) {
      // A cancelled free entry stopped at an instruction boundary inside the
      // kernel. Its registers are the only record of the work done so far.
      if ((!Single || DirectSupervisor) && E.isA<MachineInterruptedError>() &&
          (!DirectSupervisor || Prepared)) {
        if (ioctl(CPU, KVM_GET_REGS, &R) < 0 ||
            (DirectSupervisor && ioctl(CPU, KVM_GET_SREGS, &S) < 0) ||
            ioctl(CPU, KVM_GET_XSAVE, &F) < 0) {
          llvm::consumeError(std::move(E));
          Poisoned |= DirectSupervisor;
          return diagnostic::error(diagnostic::KvmState);
        }
        if (auto Decoded = decodeX64XsaveState(Next, {Bytes, sizeof(F.region)},
                                               MXCSRMask)) {
          llvm::consumeError(std::move(E));
          Poisoned |= DirectSupervisor;
          return Decoded;
        }
#define NEVERD_X64_HOST_REGISTER(Name, Field, WHP)                             \
  Next.reg(X64Register::Name) = R.Field;
#include "../../arch/x86_64/X64HostRegisters.def"
#undef NEVERD_X64_HOST_REGISTER
        if (auto Invalid = ValidateSpecial(
                nullptr, Captured && Run->exit_reason == KVM_EXIT_DEBUG)) {
          llvm::consumeError(std::move(E));
          Poisoned |= DirectSupervisor;
          return Invalid;
        }
        State = Next;
      }
      if (DirectSupervisor && !E.isA<MachineInterruptedError>() &&
          !E.isA<X64ExceptionError>())
        Poisoned = true;
      return E;
    }
    State = Next;
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::KvmRun, Control);
    // Only acknowledged, fully decoded debug exits establish reusable state.
    // Host writes or context restoration are compared against this capture;
    // faults, failed transfers and cancellation invalidate it on next entry.
    CapturedState = Next;
    CapturedSpecial = S;
    Runnable = true;
    return llvm::Error::success();
  }

  MemoryProjection &Memory;
  X64MachineState CapturedState;
  kvm_sregs CapturedSpecial{};
  uint64_t SynchronizedRegisters = 0;
  bool Runnable = false;
  bool DirectSupervisor = false, Poisoned = false;
  unsigned MaxMemorySlots = 0;
  std::vector<kvm_userspace_memory_region> NativeSlots;
};
// KVM returns zeros from CPUID until userspace installs a table. Advertise
// the host vendor and a leaf-1 identity without XSAVE or the hypervisor bit.
// Those two change the XSAVE buffer and the path a packer takes on real
// hardware; the checked floating-point decoder does not accept AVX state.
void hostCpuid(uint32_t Leaf, uint32_t Subleaf, uint32_t &EAX, uint32_t &EBX,
               uint32_t &ECX, uint32_t &EDX) {
  __asm__ volatile("cpuid"
                   : "=a"(EAX), "=b"(EBX), "=c"(ECX), "=d"(EDX)
                   : "a"(Leaf), "c"(Subleaf));
}
llvm::Error installUserCpuIdentity(int, int CPU) {
  // XSAVE, OSXSAVE, AVX and the hypervisor bit. Advertising them changes the
  // XSAVE buffer the floating-point decoder accepts.
  constexpr uint32_t Hide = (1u << 26) | (1u << 27) | (1u << 28) | (1u << 31);
  kvm_cpuid_entry2 Leaf[7]{};
  hostCpuid(0, 0, Leaf[0].eax, Leaf[0].ebx, Leaf[0].ecx, Leaf[0].edx);
  Leaf[0].function = 0;
  Leaf[0].eax = 1;
  hostCpuid(1, 0, Leaf[1].eax, Leaf[1].ebx, Leaf[1].ecx, Leaf[1].edx);
  Leaf[1].function = 1;
  Leaf[1].ecx &= ~Hide;
  for (uint32_t I = 0; I < 5; ++I) {
    auto &Entry = Leaf[2 + I];
    Entry.function = 0x80000000 + I;
    hostCpuid(Entry.function, 0, Entry.eax, Entry.ebx, Entry.ecx, Entry.edx);
  }
  Leaf[2].eax = 0x80000004;
  Leaf[3].ecx &= ~Hide;
  constexpr uint32_t Count = 7;
  std::vector<uint8_t> Bytes(sizeof(kvm_cpuid2) + Count * sizeof(Leaf[0]));
  auto *Info = reinterpret_cast<kvm_cpuid2 *>(Bytes.data());
  Info->nent = Count;
  std::memcpy(Info->entries, Leaf, sizeof(Leaf));
  if (ioctl(CPU, KVM_SET_CPUID2, Info) < 0)
    return diagnostic::unavailable(diagnostic::KvmCapabilities,
                                   BackendAvailability::MissingCapability);
  return llvm::Error::success();
}
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createKvmMachine(MemoryProjection &Memory, bool DirectSupervisor) {
  auto M = std::make_unique<KvmMachine>(Memory);
  if (auto E = M->initialize(Memory.registrations()))
    return E;
  if (auto E = installUserCpuIdentity(M->System, M->CPU))
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
  // Install the MSR policy before any vCPU entry, including the startup probe.
  if (DirectSupervisor)
    if (auto E = M->configureDirectMSRFilter())
      return E;
  M->initializeSynchronizedRegisters();
  if (auto E =
          verifyX64Machine(*M, Memory, &M->MXCSRMask, true, &M->BranchModel))
    return E;
  if (DirectSupervisor)
    if (auto E = M->enableDirectSupervisor())
      return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>> createKvmMachine(MemoryProjection &,
                                                             bool) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
