//===- LinuxProcess.cpp - Bounded ELF user process execution -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxProcess.h"

#include "../../core/ExecutionDeadline.h"
#include "../../runtime/RuntimeValues.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/ExecutionSession.h"
#include "neverd/emulation/ImageMapping.h"
#include "neverd/loader/ELF/ELFLoader.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
namespace {
enum class ServiceKind {
#define NEVERD_LINUX_SERVICE(Name, X64Number, ARMNumber, Count) Name,
#define NEVERD_LINUX_X64_SERVICE(Name, Number, Count) Name,
#include "LinuxValues.def"
#undef NEVERD_LINUX_X64_SERVICE
#undef NEVERD_LINUX_SERVICE
};
std::optional<ServiceKind> serviceKind(GuestArchitecture ISA, uint64_t Number) {
#define NEVERD_LINUX_SERVICE(Name, X64Number, ARMNumber, Count)                \
  if (Number == (ISA == GuestArchitecture::X64 ? X64Number : ARMNumber))       \
    return ServiceKind::Name;
#include "LinuxValues.def"
#undef NEVERD_LINUX_SERVICE
#define NEVERD_LINUX_X64_SERVICE(Name, Value, Count)                           \
  if (ISA == GuestArchitecture::X64 && Number == Value)                        \
    return ServiceKind::Name;
#include "LinuxValues.def"
#undef NEVERD_LINUX_X64_SERVICE
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
writeOutput(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
            const ProcessLayout &Layout, const ProcessOptions &Options,
            ProcessResult &Result) {
  const auto [FD, Address, Count, A3, A4, A5] = Event.Arguments;
  if (FD != StandardOutput && FD != StandardError)
    return std::optional<uint64_t>(uint64_t(0) - BadDescriptor);
  if (!Count)
    return std::optional<uint64_t>(0);
  if (Address >= Layout.UserLimit || Count > Layout.UserLimit - Address)
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  const uint64_t Used =
      Result.StandardOutput.size() + Result.StandardError.size();
  if (Count > Options.OutputLimit - Used) {
    Result.Stop = ProcessStopReason::OutputLimit;
    Result.Diagnostic = Output;
    return std::optional<uint64_t>();
  }
  // Captured streams are virtual byte sinks. Preserve a readable prefix on a
  // later-page fault, and return EFAULT only when no byte can be copied. Pure
  // permission queries must not poison the CPU while delivering a syscall
  // error which the guest is allowed to handle and recover from.
  uint64_t Readable = 0;
  while (Readable < Count) {
    const uint64_t Start = Address + Readable;
    const uint64_t Size =
        std::min(Count - Readable, Layout.PageSize - Start % Layout.PageSize);
    auto Access = CPU.canAccess(Start, Size, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      break;
    Readable += Size;
  }
  if (!Readable)
    return std::optional<uint64_t>(uint64_t(0) - BadAddress);
  std::string Bytes(Readable, '\0');
  if (auto E = CPU.read(Address, llvm::MutableArrayRef<uint8_t>(
                                     reinterpret_cast<uint8_t *>(Bytes.data()),
                                     Bytes.size())))
    return std::move(E);
  (FD == StandardOutput ? Result.StandardOutput : Result.StandardError)
      .append(Bytes);
  return std::optional<uint64_t>(Readable);
}
} // namespace

llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options) {
  if (!Options.Limits.Instructions || !Options.Limits.Events ||
      !Options.Limits.TimeoutMicroseconds || !Options.MemoryLimit ||
      !Options.StackSize || !Options.OutputLimit ||
      !Options.InstructionQuantum || Options.StackSize >= Options.MemoryLimit ||
      Options.StackSize > StackTop || Options.OutputLimit > Options.MemoryLimit)
    return failure(Limits);
  // Validate finite wall-clock representation before reading an input image.
  auto ValidBudget = makeExecutionDeadline(Options.Limits.TimeoutMicroseconds);
  if (!ValidBudget)
    return ValidBudget.takeError();
  ELFLoader Loader;
  auto Image = Loader.load(Path);
  if (!Image)
    return Image.takeError();
  auto Layout = processLayout(*Image);
  if (!Layout)
    return Layout.takeError();
  if (Options.StackSize % Layout->PageSize ||
      Options.Arguments.size() >
          Options.StackSize / Layout->Calls.info().WordSize ||
      Options.Environment.size() >
          Options.StackSize / Layout->Calls.info().WordSize)
    return failure(Limits);
  auto Physical = PhysicalMemory::create(Options.MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, Options.MemoryLimit);
  if (!Space)
    return Space.takeError();
  auto Plan = ImageMappingPlan::create(*Image, 0, Layout->PageSize,
                                       Options.MemoryLimit - Options.StackSize,
                                       true, ImagePagePadding::FilePages);
  if (!Plan)
    return Plan.takeError();
  const uint64_t StackBase = StackTop - Options.StackSize;
  // Setup uses a private, unpublished address space. Any failure destroys the
  // whole tentative process, so no running CPU can observe partial loading.
  for (const auto &Region : Plan->Regions) {
    const uint64_t End = Region.Address + Region.Bytes.size();
    if (Region.Address < MinimumAddress || End > Layout->UserLimit ||
        (Region.Address < StackTop + Layout->PageSize &&
         End > StackBase - Layout->PageSize))
      return failure(AddressRange);
    if (auto E =
            (*Space)->map(Region.Address, Region.Bytes.size(), Read | Write))
      return std::move(E);
    if (auto E = (*Space)->write(Region.Address, Region.Bytes))
      return std::move(E);
    if (auto E = (*Space)->protect(Region.Address, Region.Bytes.size(),
                                   Region.Permissions))
      return std::move(E);
  }
  auto Entry = (*Space)->canAccess(Plan->Entry, 1, Execute | UserAccessible);
  if (!Entry)
    return Entry.takeError();
  if (!*Entry)
    return failure(linux_model::Entry);
  const unsigned StackPermissions =
      Read | Write | UserAccessible | (Layout->ExecutableStack ? Execute : 0u);
  if (auto E = (*Space)->map(StackBase, Options.StackSize, StackPermissions))
    return std::move(E);
  auto SP =
      prepareStack(**Space, *Image, *Layout, Options, Path.filename().string());
  if (!SP)
    return SP.takeError();
  const auto Contract = Layout->Architecture == GuestArchitecture::X64
                            ? ExecutionContract::CheckedUserX64
                            : ExecutionContract::CheckedUserAArch64;
  auto Backend = createExecutionBackend(Options.Backend, Contract, *Space,
                                        Layout->Architecture);
  if (!Backend)
    return Backend.takeError();
  const auto &ABI = serviceABI(Layout->Architecture);
  if (auto E = Backend->CPU->writeRegister(ABI.StackPointer, {*SP, 0}))
    return std::move(E);
  if (auto E = Backend->CPU->writeRegister(ABI.PC, {Plan->Entry, 0}))
    return std::move(E);
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  auto Session = ExecutionSession::create(std::move(Backend->CPU), Resources);
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  ProcessResult Result{ProcessProfile::LinuxELF64, Layout->Architecture,
                       Backend->Kind, Backend->Reason};
  Result.Entry = Result.PC = Plan->Entry;
  auto RuntimeFailure = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  while (true) {
    auto Exit = (*Session)->run(Result.PC, Options.InstructionQuantum);
    if (!Exit) {
      RuntimeFailure(Exit.takeError());
      break;
    }
    Result.LastCPUExit = std::move(Exit->CPU);
    auto PC = CPU.readRegister(ABI.PC);
    if (PC)
      Result.PC = (*PC)[0];
    else {
      RuntimeFailure(PC.takeError());
      break;
    }
    if (Exit->Kind == SessionExitKind::Quantum)
      continue;
    if (Exit->Kind == SessionExitKind::InstructionLimit) {
      Result.Stop = ProcessStopReason::InstructionLimit;
      Result.Diagnostic = runtime::InstructionLimit;
      break;
    }
    if (Exit->Kind == SessionExitKind::Timeout) {
      Result.Stop = ProcessStopReason::Timeout;
      Result.Diagnostic = runtime::Timeout;
      break;
    }
    if (!Result.LastCPUExit ||
        Result.LastCPUExit->Kind != ExecutionExitKind::ServiceRequest) {
      Result.Stop = ProcessStopReason::CPUFailure;
      Result.Diagnostic =
          Result.LastCPUExit && !Result.LastCPUExit->Diagnostic.empty()
              ? Result.LastCPUExit->Diagnostic
              : UnexpectedExit;
      break;
    }
    if (!Resources->consumeEvents()) {
      Result.Stop = ProcessStopReason::EventLimit;
      Result.Diagnostic = runtime::EventLimit;
      break;
    }
    auto Request = (*Session)->takeServiceRequest();
    if (!Request) {
      RuntimeFailure(Request.takeError());
      break;
    }
    auto Event = readService(CPU, *Request);
    if (!Event) {
      RuntimeFailure(Event.takeError());
      break;
    }
    Result.Services.push_back(*Event);
    auto Kind = serviceKind(Layout->Architecture, Event->Number);
    if (!Kind) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = llvm::formatv(Service, Event->Number).str();
      break;
    }
    std::optional<uint64_t> Value;
    switch (*Kind) {
    case ServiceKind::Exit:
    case ServiceKind::ExitGroup:
      Result.Stop = ProcessStopReason::Exited;
      Result.ExitStatus = Event->Arguments[0] & ExitMask;
      break;
    case ServiceKind::GetPID:
      Value = ProcessID;
      break;
    case ServiceKind::GetTID:
      Value = ThreadID;
      break;
    case ServiceKind::ArchPrctl: {
      auto Returned = archPrctl(CPU, *Event, *Layout, Result);
      if (!Returned) {
        RuntimeFailure(Returned.takeError());
        break;
      }
      Value = *Returned;
      break;
    }
    case ServiceKind::Write: {
      auto Written = writeOutput(CPU, *Event, *Layout, Options, Result);
      if (!Written) {
        RuntimeFailure(Written.takeError());
        break;
      }
      Value = *Written;
      break;
    }
    }
    if (!Value)
      break;
    Result.Services.back().Result = *Value;
    if (auto E = returnService(CPU, *Request, *Value)) {
      RuntimeFailure(std::move(E));
      break;
    }
    Result.PC = Request->NextPC;
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  return Result;
}
} // namespace neverd::emulation::linux_model
