//===- UnicornBackend.cpp - Checked Unicorn CPU adapter -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checked Unicorn CPU adapter.
///
//===----------------------------------------------------------------------===//

#include "UnicornBackend.h"

#include "../core/ExecutionDiagnostics.h"
#include "../core/MemoryLayout.h"
#include "UnicornArchitecture.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <new>
#include <unicorn/arm64.h>
#include <unicorn/unicorn.h>
#include <unicorn/x86.h>
#include <utility>

namespace neverd::emulation {
namespace {
namespace unicornDiagnostic {
#define NEVERD_UNICORN_DIAGNOSTIC(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "UnicornDiagnostics.def"
#undef NEVERD_UNICORN_DIAGNOSTIC
} // namespace unicornDiagnostic
llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
llvm::Error check(uc_err Error, const char *Operation) {
  if (Error == UC_ERR_OK)
    return llvm::Error::success();
  return failure(std::string(Operation) +
                 unicornDiagnostic::OperationSeparator + uc_strerror(Error));
}
int registerID(CPURegister Register) {
  switch (Register) {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  case CPURegister::Arch##Name:                                                \
    return Backend;
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  case CPURegister::Arch##V##Index:                                            \
    return Backend;
#include "neverd/emulation/Registers.def"
#undef NEVERD_SCALAR_REGISTER
#undef NEVERD_VECTOR_REGISTER
  case CPURegister::Invalid:
    break;
  }
  return 0;
}
} // namespace

struct UnicornBackend::Impl {
  struct MMIORegion {
    Impl *Backend;
    uint64_t Address, Size;
    GuestMMIOCallbacks Callbacks;
  };
  uc_engine *Engine = nullptr;
  GuestArchitecture Architecture = GuestArchitecture::X64;
  int PCRegister = UC_X86_REG_RIP;
  // A separate identity survives address reuse without extending engine life.
  std::shared_ptr<const void> Identity = std::make_shared<unsigned char>(0);
  uint64_t Limit = 0;
  uint64_t Mapped = 0;
  // The adapter owns permissions for API accesses as uc_mem_read/write bypass
  // guest permissions. CPU accesses use Unicorn's corresponding page metadata.
  std::map<uint64_t, unsigned> Pages;
  std::map<uint64_t, uint8_t *> PageBacking;
  std::map<uint64_t, uint64_t> RAMAliases;
  std::vector<std::unique_ptr<uint8_t[]>> OwnedRAM;
  std::map<uint64_t, std::unique_ptr<MMIORegion>> MMIO;
  BackendHooks Hooks;
  std::vector<uc_hook> HookHandles;
  std::optional<BackendFault> FirstFault;
  std::optional<BackendFault> RecoverableFault;
  uint64_t InstructionPC = 0;
  bool Timeout = false;
  bool CallbackFailed = false;
  bool Running = false;
  bool StopRequested = false;
  bool DeviceCallbackActive = false;
  bool MMIOFailed = false;
  std::string MMIOFailure;

  ~Impl() {
    if (Engine)
      uc_close(Engine);
  }

  std::optional<BackendFaultKind> accessFault(uint64_t Address, uint64_t Size,
                                              unsigned Permission) const {
    if (!Size)
      return std::nullopt;
    if (Size - 1 > UINT64_MAX - Address)
      return BackendFaultKind::InvalidMemoryRange;
    uint64_t LastPage = (Address + Size - 1) & ~uint64_t(memory::PageSize - 1);
    for (uint64_t Page = Address & ~uint64_t(memory::PageSize - 1);;) {
      auto I = Pages.find(Page);
      if (I == Pages.end())
        return BackendFaultKind::UnmappedMemory;
      if ((I->second & Permission) != Permission)
        return BackendFaultKind::Protection;
      if (Page == LastPage)
        return std::nullopt;
      Page += memory::PageSize;
    }
  }

  bool accessible(uint64_t Address, uint64_t Size, unsigned Permission) const {
    return !accessFault(Address, Size, Permission);
  }

  MMIORegion *overlappingMMIO(uint64_t Address, uint64_t Size) const {
    if (!Size)
      return nullptr;
    for (const auto &[Base, Region] : MMIO)
      if (Address <= Base ? Base - Address < Size
                          : Address - Base < Region->Size)
        return Region.get();
    return nullptr;
  }

  llvm::Error validateRAMBacking(uint64_t Address, uint64_t Size) const {
    if (accessFault(Address, Size, 0))
      return failure(unicornDiagnostic::RAMBackingRangeIsUnmappedOrOverflowing);
    if (overlappingMMIO(Address, Size))
      return failure(unicornDiagnostic::RAMBackingAccessCannotIncludeMMIO);
    return llvm::Error::success();
  }

  bool effectsStopped() const {
    return MMIOFailed || CallbackFailed || FirstFault ||
           (Running && StopRequested);
  }

  llvm::Error deviceError() const {
    if (MMIOFailed)
      return failure(MMIOFailure.empty() ? unicornDiagnostic::MMIOCallbackFailed
                                         : MMIOFailure);
    if (CallbackFailed)
      return failure(unicornDiagnostic::ExceptionInEmulatorHook);
    return llvm::Error::success();
  }

  void failMMIO(llvm::Error Error) {
    if (!MMIOFailed) {
      MMIOFailed = true;
      MMIOFailure = llvm::toString(std::move(Error));
    } else {
      llvm::consumeError(std::move(Error));
    }
    uc_emu_stop(Engine);
  }

  llvm::Error validateMMIO(uint64_t Address, uint64_t Size, bool IsWrite) {
    auto *Region = overlappingMMIO(Address, Size);
    if (!Region)
      return llvm::Error::success();
    if (Address < Region->Address ||
        Address - Region->Address >= Region->Size ||
        Size > Region->Size - (Address - Region->Address))
      return failure(unicornDiagnostic::MMIOAccessCrossesAMappingBoundary);
    if ((Size != 1 && Size != 2 && Size != 4) || Address % Size)
      return failure(unicornDiagnostic::MMIORequiresAnAligned12Or4);
    if (DeviceCallbackActive)
      return failure(
          unicornDiagnostic::RecursiveMMIOCallbackAccessIsUnsupported);
    DeviceCallbackActive = true;
    auto Reset = llvm::scope_exit([&] { DeviceCallbackActive = false; });
    return Region->Callbacks.Validate(Address - Region->Address, Size, IsWrite);
  }

  void preflightMMIO(uint64_t Address, uint64_t Size, bool IsWrite) {
    if (auto E = validateMMIO(Address, Size, IsWrite))
      failMMIO(std::move(E));
  }

  static uint64_t mmioRead(uc_engine *, uint64_t Offset, unsigned Size,
                           void *Opaque) noexcept {
    auto &Region = *static_cast<MMIORegion *>(Opaque);
    auto &S = *Region.Backend;
    uint64_t Value = 0;
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Region.Address + Offset, Size, false);
      if (S.effectsStopped())
        return;
      S.DeviceCallbackActive = true;
      auto Reset = llvm::scope_exit([&] { S.DeviceCallbackActive = false; });
      auto Result = Region.Callbacks.Read(Offset, Size);
      if (!Result) {
        S.failMMIO(Result.takeError());
        return;
      }
      Value = *Result & ((uint64_t(1) << (Size * 8)) - 1);
    });
    return Value;
  }

  static void mmioWrite(uc_engine *, uint64_t Offset, unsigned Size,
                        uint64_t Value, void *Opaque) noexcept {
    auto &Region = *static_cast<MMIORegion *>(Opaque);
    auto &S = *Region.Backend;
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Region.Address + Offset, Size, true);
      if (S.effectsStopped())
        return;
      S.DeviceCallbackActive = true;
      auto Reset = llvm::scope_exit([&] { S.DeviceCallbackActive = false; });
      if (auto E = Region.Callbacks.Write(
              Offset, Size, Value & ((uint64_t(1) << (Size * 8)) - 1)))
        S.failMMIO(std::move(E));
    });
  }

  uint64_t currentPC() const noexcept {
    uint64_t PC = InstructionPC;
    // Capture inside the fault boundary, before a hook can change the CPU.
    if (uc_reg_read(Engine, PCRegister, &PC) != UC_ERR_OK)
      return InstructionPC;
    return PC;
  }

  void retain(BackendFault Fault) noexcept {
    if (!FirstFault)
      FirstFault = Fault;
  }

  void memoryFault(BackendFaultKind Kind, BackendAccessKind Access,
                   uint64_t Address, uint64_t Size) noexcept {
    retain({Kind, currentPC(), Address, Size, Access, std::nullopt});
  }

  template <typename F> void invoke(F &&Function) noexcept {
    try {
      Function();
    } catch (...) {
      // A hook may have failed because allocation itself failed. Keep this
      // noexcept callback free of further allocation; diagnose after Unicorn
      // returns to the ordinary C++ boundary.
      CallbackFailed = true;
      uc_emu_stop(Engine);
    }
  }
  static void code(uc_engine *, uint64_t Address, uint32_t Size, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.InstructionPC = Address;
    S.invoke([&] {
      if (S.Hooks.Instruction)
        S.Hooks.Instruction(Address, Size);
    });
  }
  static void write(uc_engine *, uc_mem_type, uint64_t Address, int Size,
                    int64_t Value, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Address, Size, true);
      if (S.effectsStopped())
        return;
      if (S.Hooks.Write)
        S.Hooks.Write(Address, Size, uint64_t(Value));
    });
  }
  static void read(uc_engine *, uc_mem_type, uint64_t Address, int Size,
                   int64_t, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.invoke([&] {
      if (S.effectsStopped())
        return;
      S.preflightMMIO(Address, Size, false);
      if (S.effectsStopped())
        return;
      if (S.Hooks.Read)
        S.Hooks.Read(Address, Size);
    });
  }
  static bool fault(uc_engine *, uc_mem_type Type, uint64_t Address, int Size,
                    int64_t, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    BackendAccessKind Access;
    BackendFaultKind Kind;
    switch (Type) {
#define NEVERD_UNICORN_MEMORY_FAULT(Event, FaultKind, AccessKind)              \
  case Event:                                                                  \
    Kind = BackendFaultKind::FaultKind;                                        \
    Access = BackendAccessKind::AccessKind;                                    \
    break;
#include "UnicornFaults.def"
#undef NEVERD_UNICORN_MEMORY_FAULT
    default:
      S.CallbackFailed = true;
      uc_emu_stop(S.Engine);
      return false;
    }
    BackendFault Fault{
        Kind,    S.currentPC(),
        Address, Size > 0 ? std::optional<uint64_t>(Size) : std::nullopt,
        Access,  std::nullopt};
    S.invoke([&] {
      if (!S.effectsStopped() && S.Hooks.RecoverableFault &&
          S.Hooks.RecoverableFault(Fault)) {
        S.RecoverableFault = Fault;
        uc_emu_stop(S.Engine);
      }
    });
    if (S.RecoverableFault)
      return false;
    S.retain(Fault);
    S.invoke([&] {
      if (S.Hooks.Fault)
        S.Hooks.Fault(Address, Size > 0 ? uint32_t(Size) : 0,
                      backendAccessKindName(Access));
    });
    return false;
  }
  static void interrupt(uc_engine *, uint32_t Number, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    // RIP may already follow INT3. The code hook identifies the instruction
    // that raised the event, including a synchronous CPU exception such as #DE.
    S.retain({BackendFaultKind::Interrupt, S.InstructionPC, std::nullopt,
              std::nullopt, std::nullopt, Number});
    S.invoke([&] {
      if (S.Hooks.Interrupt)
        S.Hooks.Interrupt(Number);
    });
    uc_emu_stop(S.Engine);
  }
  static bool invalidInstruction(uc_engine *, void *Opaque) {
    auto &S = *static_cast<Impl *>(Opaque);
    S.retain({BackendFaultKind::InvalidInstruction, S.currentPC(), std::nullopt,
              std::nullopt, std::nullopt, std::nullopt});
    S.invoke([&] {
      if (S.Hooks.InvalidInstruction)
        S.Hooks.InvalidInstruction();
    });
    return false;
  }
};

struct UnicornContext final : BackendContext::Storage {
  uc_context *Context = nullptr;

  ~UnicornContext() override {
    if (Context)
      uc_context_free(Context);
  }
};

UnicornBackend::UnicornBackend(std::unique_ptr<Impl> State)
    : State(std::move(State)) {}
UnicornBackend::~UnicornBackend() = default;

llvm::Expected<std::unique_ptr<UnicornBackend>>
UnicornBackend::create(uint64_t MemoryLimit, GuestArchitecture Architecture) {
  auto S = std::make_unique<Impl>();
  S->Limit = MemoryLimit;
  S->Architecture = Architecture;
  uc_arch Arch;
  uc_mode Mode;
  switch (Architecture) {
  case GuestArchitecture::X64:
    Arch = UC_ARCH_X86;
    Mode = UC_MODE_64;
    break;
  case GuestArchitecture::AArch64:
    Arch = UC_ARCH_ARM64;
    Mode = UC_MODE_ARM;
    S->PCRegister = UC_ARM64_REG_PC;
    break;
  default:
    return diagnostic::error(diagnostic::Architecture);
  }
  if (auto E =
          check(uc_open(Arch, Mode, &S->Engine), diagnostic::UnicornCreate))
    return std::move(E);
  // GuestMemory maps virtual addresses directly without Windows page tables.
  // The CPU TLB applies its physical address width even with paging disabled,
  // which truncates canonical kernel addresses. Unicorn's virtual TLB keeps
  // these addresses intact while retaining the mapped page permissions.
  if (auto E = check(uc_ctl_tlb_mode(S->Engine, UC_TLB_VIRTUAL),
                     unicornDiagnostic::ConfigureGuestVirtualAddressSpace))
    return std::move(E);
  // Scheduling exchanges CPU state while every thread observes the same live
  // address space. Never enable Unicorn's optional memory snapshot mode.
  if (auto E = check(uc_ctl_context_mode(S->Engine, UC_CTL_CONTEXT_CPU),
                     unicornDiagnostic::ConfigureCPUContextContents))
    return std::move(E);
  if (auto E = initializeUnicornArchitecture(S->Engine, Architecture))
    return std::move(E);
  auto Backend =
      std::unique_ptr<UnicornBackend>(new UnicornBackend(std::move(S)));
  // Core fault capture must also work without optional tracing callbacks.
  if (auto E = Backend->installHooks({}))
    return std::move(E);
  return Backend;
}

llvm::Error UnicornBackend::map(uint64_t Address, uint64_t Size,
                                unsigned Permissions) {
  if (!Size || (Address & (memory::PageSize - 1)) ||
      (Size & (memory::PageSize - 1)) || Size - 1 > UINT64_MAX - Address ||
      (Permissions & ~(Read | Write | Execute)) ||
      Size > State->Limit - State->Mapped)
    return failure(unicornDiagnostic::InvalidGuestMappingOrMemoryLimitExceeded);
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    if (State->Pages.count(Address + Offset))
      return failure(unicornDiagnostic::OverlappingGuestMapping);
  auto Allocation = std::unique_ptr<uint8_t[]>(
      new (std::nothrow) uint8_t[Size + memory::PageSize - 1]());
  if (!Allocation)
    return llvm::make_error<GuestMemoryLimitError>();
  auto *Raw = reinterpret_cast<uint8_t *>(
      (reinterpret_cast<uintptr_t>(Allocation.get()) + memory::PageSize - 1) &
      ~(uintptr_t(memory::PageSize) - 1));
  if (auto E =
          check(uc_mem_map_ptr(State->Engine, Address, Size, Permissions, Raw),
                unicornDiagnostic::MapGuestMemory))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize) {
    State->Pages.emplace(Address + Offset, Permissions);
    State->PageBacking.emplace(Address + Offset, Raw + Offset);
  }
  State->OwnedRAM.push_back(std::move(Allocation));
  State->Mapped += Size;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::mapAlias(uint64_t Address, uint64_t Source,
                                     uint64_t Size, unsigned Permissions) {
  return replaceAliases({}, {{Address, Source, Size, Permissions}});
}

llvm::Error UnicornBackend::unmapAlias(uint64_t Address, uint64_t Size) {
  return replaceAliases({{Address, Size}}, {});
}

llvm::Error
UnicornBackend::replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                               llvm::ArrayRef<GuestAliasMapping> Add) {
  if (State->Running || State->DeviceCallbackActive || State->effectsStopped())
    return failure(
        unicornDiagnostic::CannotReplaceRAMAliasesDuringExecutionACallback);
  std::map<uint64_t, uint64_t> Retiring;
  uint64_t FinalMapped = State->Mapped;
  for (const auto &Range : Remove) {
    auto Alias = State->RAMAliases.find(Range.Address);
    if (Alias == State->RAMAliases.end() || Alias->second != Range.Size ||
        !Retiring.emplace(Range.Address, Range.Size).second)
      return failure(unicornDiagnostic::
                         RAMAliasRemovalRequiresUniqueExactCompleteMappings);
    FinalMapped -= Range.Size;
  }
  auto RetiresPage = [&](uint64_t Page) {
    auto Next = Retiring.upper_bound(Page);
    if (Next == Retiring.begin())
      return false;
    const auto &Range = *std::prev(Next);
    return Page - Range.first < Range.second;
  };
  struct PreparedAlias {
    GuestAliasMapping Mapping;
    uint8_t *Backing;
  };
  std::vector<PreparedAlias> Prepared;
  std::map<uint64_t, uint64_t> Destinations;
  for (const auto &Mapping : Add) {
    const auto [Address, Source, Size, Permissions] = Mapping;
    if (!Size || (Address & (memory::PageSize - 1)) ||
        (Source & (memory::PageSize - 1)) || (Size & (memory::PageSize - 1)) ||
        Size - 1 > UINT64_MAX - Address || Size - 1 > UINT64_MAX - Source ||
        (Permissions & ~(Read | Write | Execute)))
      return failure(unicornDiagnostic::InvalidSharedRAMAlias);
    if (Size > State->Limit - FinalMapped)
      return llvm::make_error<GuestMemoryLimitError>();
    FinalMapped += Size;
    auto Next = Destinations.lower_bound(Address);
    if ((Next != Destinations.end() && Next->first - Address < Size) ||
        (Next != Destinations.begin() &&
         Address - std::prev(Next)->first < std::prev(Next)->second))
      return failure(unicornDiagnostic::ReplacementRAMAliasesOverlapEachOther);
    Destinations.emplace_hint(Next, Address, Size);
    auto First = State->PageBacking.find(Source);
    if (First == State->PageBacking.end())
      return failure(unicornDiagnostic::SharedRAMAliasHasNoSourceBacking);
    for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize) {
      auto Page = State->PageBacking.find(Source + Offset);
      if (RetiresPage(Source + Offset))
        return failure(
            unicornDiagnostic::ReplacementRAMAliasSourceIsBeingRetired);
      if ((State->Pages.count(Address + Offset) &&
           !RetiresPage(Address + Offset)) ||
          Page == State->PageBacking.end() ||
          reinterpret_cast<uintptr_t>(Page->second) !=
              reinterpret_cast<uintptr_t>(First->second) + Offset)
        return failure(
            unicornDiagnostic::SharedRAMAliasOverlapsOrCrossesSourceBacking);
    }
    Prepared.push_back({Mapping, First->second});
  }
  auto CheckMutation = [&](uc_err Status, const char *Operation,
                           uint64_t Address, uint64_t Size) -> llvm::Error {
    if (Status != UC_ERR_OK) {
      // All predictable failures were checked before the first mutation. An
      // unexpected engine failure must not leave a resumable partial switch.
      State->FirstFault = BackendFault{BackendFaultKind::UnhandledException,
                                       State->InstructionPC, Address, Size};
      return check(Status, Operation);
    }
    return llvm::Error::success();
  };
  for (const auto &Range : Remove) {
    if (auto E = CheckMutation(
            uc_mem_unmap(State->Engine, Range.Address, Range.Size),
            unicornDiagnostic::UnmapSharedGuestMemory, Range.Address,
            Range.Size))
      return E;
    for (uint64_t Offset = 0; Offset < Range.Size; Offset += memory::PageSize) {
      State->Pages.erase(Range.Address + Offset);
      State->PageBacking.erase(Range.Address + Offset);
    }
    State->RAMAliases.erase(Range.Address);
    State->Mapped -= Range.Size;
  }
  for (const auto &Alias : Prepared) {
    const auto [Address, Source, Size, Permissions] = Alias.Mapping;
    if (auto E = CheckMutation(uc_mem_map_ptr(State->Engine, Address, Size,
                                              Permissions, Alias.Backing),
                               unicornDiagnostic::MapSharedGuestMemory, Address,
                               Size))
      return E;
    for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize) {
      State->Pages.emplace(Address + Offset, Permissions);
      State->PageBacking.emplace(Address + Offset, Alias.Backing + Offset);
    }
    State->RAMAliases.emplace(Address, Size);
    State->Mapped += Size;
  }
  return llvm::Error::success();
}

llvm::Error UnicornBackend::protect(uint64_t Address, uint64_t Size,
                                    unsigned Permissions) {
  if (!Size || (Address & (memory::PageSize - 1)) ||
      (Size & (memory::PageSize - 1)) ||
      (Permissions & ~(Read | Write | Execute)) ||
      !State->accessible(Address, Size, 0))
    return failure(unicornDiagnostic::InvalidGuestProtectionRange);
  if (State->overlappingMMIO(Address, Size))
    return failure(unicornDiagnostic::ChangingMMIOPagePermissionsIsUnsupported);
  if (auto E = check(uc_mem_protect(State->Engine, Address, Size, Permissions),
                     unicornDiagnostic::ProtectGuestMemory))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    State->Pages[Address + Offset] = Permissions;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::mapMMIO(uint64_t Address, uint64_t Size,
                                    GuestMMIOCallbacks Callbacks) {
  if (State->Running || State->DeviceCallbackActive)
    return failure(unicornDiagnostic::CannotMapMMIODuringGuestExecutionOrA);
  if (!Size || (Address & (memory::PageSize - 1)) ||
      (Size & (memory::PageSize - 1)) || Size - 1 > UINT64_MAX - Address)
    return failure(unicornDiagnostic::InvalidMMIOMapping);
  if (Size > State->Limit - State->Mapped)
    return llvm::make_error<GuestMemoryLimitError>();
  if (!Callbacks.Validate || !Callbacks.Read || !Callbacks.Write)
    return failure(
        unicornDiagnostic::MMIOMappingRequiresValidateReadAndWriteCallbacks);
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    if (State->Pages.count(Address + Offset))
      return failure(unicornDiagnostic::OverlappingGuestMapping);
  auto Region = std::make_unique<Impl::MMIORegion>(
      Impl::MMIORegion{State.get(), Address, Size, std::move(Callbacks)});
  auto *Identity = Region.get();
  State->MMIO.emplace(Address, std::move(Region));
  if (auto E = check(uc_mmio_map(State->Engine, Address, Size, Impl::mmioRead,
                                 Identity, Impl::mmioWrite, Identity),
                     unicornDiagnostic::MapGuestMMIO)) {
    State->MMIO.erase(Address);
    return E;
  }
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    State->Pages.emplace(Address + Offset, Read | Write);
  State->Mapped += Size;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::unmapMMIO(uint64_t Address, uint64_t Size) {
  if (State->Running || State->DeviceCallbackActive)
    return failure(unicornDiagnostic::CannotUnmapMMIODuringGuestExecutionOrA);
  auto I = State->MMIO.find(Address);
  if (I == State->MMIO.end() || I->second->Size != Size)
    return failure(unicornDiagnostic::MMIOUnmapRequiresOneExactCompleteMapping);
  if (auto E = check(uc_mem_unmap(State->Engine, Address, Size),
                     unicornDiagnostic::UnmapGuestMMIO))
    return E;
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    State->Pages.erase(Address + Offset);
  State->Mapped -= Size;
  State->MMIO.erase(I);
  return llvm::Error::success();
}

llvm::Error UnicornBackend::read(uint64_t Address,
                                 llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto E = State->deviceError())
    return E;
  if (auto Kind = State->accessFault(Address, Bytes.size(), Read)) {
    State->memoryFault(*Kind, BackendAccessKind::Read, Address, Bytes.size());
    return failure(std::string(unicornDiagnostic::GuestReadFaultAt0x) +
                   llvm::utohexstr(Address));
  }
  if (State->overlappingMMIO(Address, Bytes.size()) && State->effectsStopped())
    return failure(unicornDiagnostic::CannotAccessMMIOOnAStoppedOrFaulted);
  State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), false); });
  if (auto E = State->deviceError())
    return E;
  auto Status = Bytes.empty() ? UC_ERR_OK
                              : uc_mem_read(State->Engine, Address,
                                            Bytes.data(), Bytes.size());
  if (auto E = State->deviceError())
    return E;
  return check(Status, unicornDiagnostic::ReadGuestMemory);
}
llvm::Error UnicornBackend::write(uint64_t Address,
                                  llvm::ArrayRef<uint8_t> Bytes) {
  if (auto E = State->deviceError())
    return E;
  if (auto Kind = State->accessFault(Address, Bytes.size(), Write)) {
    State->memoryFault(*Kind, BackendAccessKind::Write, Address, Bytes.size());
    return failure(std::string(unicornDiagnostic::GuestWriteFaultAt0x) +
                   llvm::utohexstr(Address));
  }
  if (State->overlappingMMIO(Address, Bytes.size()) && State->effectsStopped())
    return failure(unicornDiagnostic::CannotAccessMMIOOnAStoppedOrFaulted);
  State->invoke([&] { State->preflightMMIO(Address, Bytes.size(), true); });
  if (auto E = State->deviceError())
    return E;
  auto Status = Bytes.empty() ? UC_ERR_OK
                              : uc_mem_write(State->Engine, Address,
                                             Bytes.data(), Bytes.size());
  if (auto E = State->deviceError())
    return E;
  return check(Status, unicornDiagnostic::WriteGuestMemory);
}
llvm::Error UnicornBackend::fetch(uint64_t Address,
                                  llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto Kind = State->accessFault(Address, Bytes.size(), Execute)) {
    State->memoryFault(*Kind, BackendAccessKind::Execute, Address,
                       Bytes.size());
    return failure(std::string(unicornDiagnostic::GuestFetchFaultAt0x) +
                   llvm::utohexstr(Address));
  }
  return check(uc_mem_read(State->Engine, Address, Bytes.data(), Bytes.size()),
               unicornDiagnostic::FetchGuestInstruction);
}

llvm::Error UnicornBackend::validateBacking(uint64_t Address,
                                            uint64_t Size) const {
  if (State->Running || State->DeviceCallbackActive)
    return failure(
        unicornDiagnostic::RAMBackingAccessRequiresAStoppedCPUWithout);
  if (State->effectsStopped())
    return failure(unicornDiagnostic::CannotAccessRAMBackingOnAFaultedCPU);
  return State->validateRAMBacking(Address, Size);
}

llvm::Error
UnicornBackend::snapshotBacking(uint64_t Address,
                                llvm::MutableArrayRef<uint8_t> Bytes) {
  if (State->Running || State->DeviceCallbackActive)
    return failure(unicornDiagnostic::RAMSnapshotRequiresAStoppedCPUWithoutAn);
  if (auto E = State->validateRAMBacking(Address, Bytes.size()))
    return E;
  // Read adapter-owned RAM directly, without reentering a faulted engine.
  // Each page may belong to a separate allocation or shared virtual alias.
  while (!Bytes.empty()) {
    const uint64_t Offset = Address & (memory::PageSize - 1);
    const auto *Backing = State->PageBacking.at(Address - Offset);
    const size_t Count =
        std::min<uint64_t>(Bytes.size(), memory::PageSize - Offset);
    std::copy_n(Backing + Offset, Count, Bytes.begin());
    Bytes = Bytes.drop_front(Count);
    Address += Count;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> UnicornBackend::canAccess(uint64_t Address, uint64_t Size,
                                               unsigned Permissions) const {
  if ((Permissions & ~(Read | Write | Execute)) ||
      State->DeviceCallbackActive || State->effectsStopped())
    return failure(
        unicornDiagnostic::CPUAccessPreflightRequiresValidPermissionsAndA);
  return !State->accessFault(Address, Size, Permissions) &&
         !State->overlappingMMIO(Address, Size);
}

llvm::Error UnicornBackend::readBacking(uint64_t Address,
                                        llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(Address, Bytes.size()))
    return E;
  const uc_err Status = Bytes.empty() ? UC_ERR_OK
                                      : uc_mem_read(State->Engine, Address,
                                                    Bytes.data(), Bytes.size());
  if (Status != UC_ERR_OK)
    State->memoryFault(BackendFaultKind::UnhandledException,
                       BackendAccessKind::Read, Address, Bytes.size());
  return check(Status, unicornDiagnostic::ReadRAMBacking);
}

llvm::Error UnicornBackend::writeBacking(uint64_t Address,
                                         llvm::ArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(Address, Bytes.size()))
    return E;
  const uc_err Status =
      Bytes.empty()
          ? UC_ERR_OK
          : uc_mem_write(State->Engine, Address, Bytes.data(), Bytes.size());
  // Unicorn's host write bypasses CPU permissions internally. An unexpected
  // failure does not promise a usable engine or a restored internal readonly
  // state, so preserve the fault and prohibit resume rather than retrying.
  if (Status != UC_ERR_OK)
    State->memoryFault(BackendFaultKind::UnhandledException,
                       BackendAccessKind::Write, Address, Bytes.size());
  return check(Status, unicornDiagnostic::WriteRAMBacking);
}
GuestArchitecture UnicornBackend::architecture() const {
  return State->Architecture;
}
llvm::Expected<RegisterValue> UnicornBackend::readRegister(CPURegister R) {
  if (!registerMatches(R, architecture()))
    return diagnostic::error(diagnostic::Register);
  RegisterValue V{};
  if (auto E = check(uc_reg_read(State->Engine, registerID(R), V.data()),
                     diagnostic::UnicornReadRegister))
    return std::move(E);
  return V;
}
llvm::Error UnicornBackend::writeRegister(CPURegister R,
                                          const RegisterValue &V) {
  if (!registerMatches(R, architecture()) || (registerWidth(R) <= 64 && V[1]) ||
      (registerWidth(R) == 32 && V[0] > UINT32_MAX))
    return diagnostic::error(diagnostic::Register);
  return check(uc_reg_write(State->Engine, registerID(R), V.data()),
               diagnostic::UnicornWriteRegister);
}

llvm::Expected<std::unique_ptr<BackendContext>> UnicornBackend::saveContext() {
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed)
    return failure(unicornDiagnostic::CannotSaveAFaultedCPUInstance);
  auto Saved = std::make_unique<UnicornContext>();
  Saved->Owner = State->Identity;
  if (auto E = check(uc_context_alloc(State->Engine, &Saved->Context),
                     unicornDiagnostic::AllocateCPUContext))
    return std::move(E);
  auto Context =
      std::unique_ptr<BackendContext>(new BackendContext(std::move(Saved)));
  if (auto E = saveContext(*Context))
    return std::move(E);
  return Context;
}

llvm::Error UnicornBackend::saveContext(BackendContext &Context) {
  if (!Context.State || Context.State->Owner.expired())
    return failure(unicornDiagnostic::CannotSaveToAnExpiredCPUContext);
  if (Context.State->Owner.lock() != State->Identity)
    return failure(
        unicornDiagnostic::CPUContextBelongsToAnotherBackendInstance);
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed)
    return failure(unicornDiagnostic::CannotSaveAFaultedCPUInstance);
  return check(
      uc_context_save(State->Engine,
                      static_cast<UnicornContext &>(*Context.State).Context),
      unicornDiagnostic::SaveCPUContext);
}

llvm::Error UnicornBackend::restoreContext(const BackendContext &Context) {
  if (!Context.State || Context.State->Owner.expired())
    return failure(unicornDiagnostic::CannotRestoreAnExpiredCPUContext);
  if (Context.State->Owner.lock() != State->Identity)
    return failure(
        unicornDiagnostic::CPUContextBelongsToAnotherBackendInstance);
  if (State->FirstFault || State->CallbackFailed || State->MMIOFailed)
    return failure(unicornDiagnostic::CannotRestoreAFaultedCPUInstance);
  if (State->Running)
    return failure(
        unicornDiagnostic::CannotRestoreCPUContextDuringGuestExecution);
  if (auto E = check(uc_context_restore(
                         State->Engine,
                         static_cast<UnicornContext &>(*Context.State).Context),
                     unicornDiagnostic::RestoreCPUContext))
    return E;
  State->InstructionPC = State->currentPC();
  State->Timeout = false;
  return llvm::Error::success();
}

llvm::Error UnicornBackend::installHooks(BackendHooks Hooks) {
  State->Hooks = std::move(Hooks);
  // Replacing observers must not duplicate the underlying hooks.
  if (!State->HookHandles.empty())
    return llvm::Error::success();
  const std::pair<int, void *> Entries[] = {
      {UC_HOOK_CODE, reinterpret_cast<void *>(Impl::code)},
      {UC_HOOK_MEM_READ, reinterpret_cast<void *>(Impl::read)},
      {UC_HOOK_MEM_WRITE, reinterpret_cast<void *>(Impl::write)},
      {UC_HOOK_MEM_INVALID, reinterpret_cast<void *>(Impl::fault)},
      {UC_HOOK_INTR, reinterpret_cast<void *>(Impl::interrupt)},
      {UC_HOOK_INSN_INVALID,
       reinterpret_cast<void *>(Impl::invalidInstruction)}};
  for (const auto &Entry : Entries) {
    uc_hook Hook = 0;
    if (auto E = check(uc_hook_add(State->Engine, &Hook, Entry.first,
                                   Entry.second, State.get(), 1, 0),
                       unicornDiagnostic::InstallCPUHook))
      return E;
    State->HookHandles.push_back(Hook);
  }
  return llvm::Error::success();
}
llvm::Error UnicornBackend::run(uint64_t PC, uint64_t TimeoutMicroseconds) {
  if (auto E = State->deviceError())
    return E;
  if (State->FirstFault || State->CallbackFailed || State->RecoverableFault)
    return failure(unicornDiagnostic::CannotResumeAFaultedCPUInstance);
  if (State->Running)
    return failure(unicornDiagnostic::CannotRecursivelyExecuteACPUInstance);
  // Host writes through another virtual alias can change executable backing.
  // Flush cached translations at this stopped-CPU boundary before resuming.
  if (auto E = check(uc_ctl_flush_tb(State->Engine),
                     unicornDiagnostic::InvalidateCPUTranslations))
    return E;
  State->InstructionPC = PC;
  State->Timeout = false;
  State->StopRequested = false;
  State->Running = true;
  uc_err Status =
      uc_emu_start(State->Engine, PC, UINT64_MAX, TimeoutMicroseconds, 0);
  State->Running = false;
  if (auto E = State->deviceError())
    return E;
  if (State->RecoverableFault) {
    // Unicorn reports the original memory error even after the hook stops the
    // instruction. Only that exact hook-admitted event may be resumed by a
    // caller-supplied exception transfer; all other errors remain terminal.
    if (State->CallbackFailed || State->FirstFault ||
        (Status != UC_ERR_OK && Status != UC_ERR_READ_UNMAPPED &&
         Status != UC_ERR_WRITE_UNMAPPED && Status != UC_ERR_READ_PROT &&
         Status != UC_ERR_WRITE_PROT)) {
      State->retain(*State->RecoverableFault);
      State->RecoverableFault.reset();
      return check(Status, unicornDiagnostic::ExecuteGuestAfterMemoryException);
    }
    return llvm::Error::success();
  }
  if (Status == UC_ERR_INSN_INVALID || Status == UC_ERR_EXCEPTION)
    State->retain({Status == UC_ERR_INSN_INVALID
                       ? BackendFaultKind::InvalidInstruction
                       : BackendFaultKind::UnhandledException,
                   State->currentPC(), std::nullopt, std::nullopt, std::nullopt,
                   std::nullopt});
  size_t TimedOut = 0;
  if (auto E = check(uc_query(State->Engine, UC_QUERY_TIMEOUT, &TimedOut),
                     unicornDiagnostic::QueryCPUTimeout))
    return E;
  State->Timeout = TimedOut != 0;
  if (State->CallbackFailed)
    return failure(unicornDiagnostic::ExceptionInEmulatorHook);
  return check(Status, unicornDiagnostic::ExecuteGuest);
}
bool UnicornBackend::timedOut() const { return State->Timeout; }
void UnicornBackend::stop() {
  if (State->Running)
    State->StopRequested = true;
  uc_emu_stop(State->Engine);
}
bool UnicornBackend::hasMemoryFault() const {
  return State->FirstFault && State->FirstFault->Access.has_value();
}
bool UnicornBackend::hasDeviceError() const { return State->MMIOFailed; }
std::optional<BackendFault> UnicornBackend::fault() const {
  return State->FirstFault;
}
std::optional<BackendFault> UnicornBackend::takeRecoverableFault() {
  auto Fault = State->RecoverableFault;
  State->RecoverableFault.reset();
  return Fault;
}
bool UnicornBackend::executable(uint64_t Address) const {
  return State->accessible(Address, 1, Execute);
}
} // namespace neverd::emulation
