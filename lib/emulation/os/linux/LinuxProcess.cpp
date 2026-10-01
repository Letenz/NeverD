//===- LinuxProcess.cpp - Bounded ELF user process execution -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxProcess.h"

#include "../../core/ExecutionDeadline.h"
#include "../../runtime/RuntimeValues.h"
#include "LinuxMemory.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/ExecutionSession.h"
#include "neverd/emulation/ImageMapping.h"
#include "neverd/loader/ELF/ELFLoader.h"

namespace neverd::emulation::linux_model {
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         const ProcessOptions &Options) {
  if (Options.Android)
    return failure("Android options require the Android native profile");
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
  auto Plan = ImageMappingPlan::create(
      *Image, Layout->LoadBias, Layout->PageSize,
      Options.MemoryLimit - Options.StackSize, true,
      ImagePagePadding::FilePages, ImageByteSource::OriginalFile);
  if (!Plan)
    return Plan.takeError();
  const uint64_t StackBase = StackTop - Options.StackSize;
  uint64_t InitialBreak = MinimumAddress;
  // Setup uses a private, unpublished address space. Any failure destroys the
  // whole tentative process, so no running CPU can observe partial loading.
  for (const auto &Region : Plan->Regions) {
    const uint64_t End = Region.Address + Region.Bytes.size();
    InitialBreak = std::max(InitialBreak, End);
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
  LinuxMemory Memory(**Space, *Layout, InitialBreak, Options);
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
    auto Returned =
        handleService(CPU, Memory, *Event, *Layout, Options, Result);
    if (!Returned) {
      RuntimeFailure(Returned.takeError());
      break;
    }
    auto Value = *Returned;
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
