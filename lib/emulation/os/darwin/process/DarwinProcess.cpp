//===- DarwinProcess.cpp - Bounded macOS/iOS process continuations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinProcess.h"

#include "../../../runtime/RuntimeValues.h"
#include "../kernel/DarwinEntropy.h"
#include "../kernel/DarwinFiles.h"
#include "../kernel/DarwinMemory.h"
#include "../kernel/DarwinSystem.h"
#include "DarwinCallbacks.h"
#include "DarwinLibSystem.h"
#include "DarwinTLS.h"

#include "neverd/emulation/ExecutionSession.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
using namespace value;
char PreparationTimeout::ID = 0;
void PreparationTimeout::log(llvm::raw_ostream &OS) const {
  OS << diagnostic::PreparationDeadline;
}
namespace {
llvm::Expected<ProcessResult>
preparationTimeout(ProfileSpec Profile, const ProcessOptions &Options,
                   GuestArchitecture Architecture, uint64_t Entry,
                   const BackendSelection *Selected = nullptr) {
  // Resolve and construct the real requested transport even when preparation
  // used the deadline. A timeout must not hide an unavailable backend or claim
  // a backend that the process could not create. No guest runs on this path.
  std::optional<BackendSelection> Created;
  if (!Selected) {
    auto Backend =
        createExecutionBackend(Options.Backend,
                               Architecture == GuestArchitecture::X64
                                   ? ExecutionContract::CheckedUserX64
                                   : ExecutionContract::CheckedUserAArch64,
                               Options.MemoryLimit, Architecture);
    if (!Backend)
      return Backend.takeError();
    Created = std::move(*Backend);
    Selected = &*Created;
  }
  ProcessResult Result{Profile.Profile, Architecture, Selected->Kind,
                       Selected->Reason};
  Result.Entry = Result.PC = Entry;
  Result.Stop = ProcessStopReason::Timeout;
  Result.Diagnostic = runtime::Timeout;
  return Result;
}
llvm::Expected<ProcessResult>
preparationFailure(llvm::Error Error, ProfileSpec Profile,
                   const ProcessOptions &Options) {
  std::optional<std::pair<GuestArchitecture, uint64_t>> Expired;
  auto Other = llvm::handleErrors(std::move(Error), [&](PreparationTimeout &E) {
    Expired = {E.Architecture, E.Entry};
  });
  if (Other)
    return std::move(Other);
  assert(Expired);
  return preparationTimeout(Profile, Options, Expired->first, Expired->second);
}
} // namespace
llvm::Expected<ProcessResult> runProcess(const std::filesystem::path &Path,
                                         ProfileSpec Profile,
                                         const ProcessOptions &Options) {
  if (Options.Android || !Options.Limits.Instructions ||
      !Options.Limits.Events || !Options.Limits.TimeoutMicroseconds ||
      !Options.MemoryLimit || !Options.StackSize || !Options.OutputLimit ||
      !Options.InstructionQuantum || Options.StackSize > MaxStackSize ||
      Options.StackSize >= Options.MemoryLimit ||
      Options.OutputLimit > Options.MemoryLimit ||
      Options.Arguments.size() > Options.StackSize / 8 ||
      Options.Environment.size() > Options.StackSize / 8)
    return failure(diagnostic::ProcessLimits);
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  auto Image = loadImage(Path, Profile, Options, Resources.get());
  if (!Image)
    return preparationFailure(Image.takeError(), Profile, Options);
  const uint64_t MemoryLimit = Options.MemoryLimit - Image->MetadataBytes;
  auto Physical = PhysicalMemory::create(MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, MemoryLimit);
  if (!Space)
    return Space.takeError();
  // An initial segment must not retain a multi-page physical owner after a
  // partial munmap. Image, stack and anonymous mappings share this invariant.
  auto MapPages = [&](uint64_t Address, uint64_t Size,
                      unsigned Permissions) -> llvm::Error {
    for (uint64_t Offset = 0; Offset < Size; Offset += Image->Memory.PageSize) {
      if (!Resources->remainingMicroseconds())
        return llvm::make_error<PreparationTimeout>(Image->Architecture,
                                                    Image->Plan.Entry);
      if (auto E = (*Space)->map(Address + Offset, Image->Memory.PageSize,
                                 Permissions))
        return E;
    }
    return llvm::Error::success();
  };
  for (const auto &Region : Image->Plan.Regions) {
    if (auto E = MapPages(Region.Address, Region.Bytes.size(), Read | Write))
      return preparationFailure(std::move(E), Profile, Options);
    if (auto E = (*Space)->write(Region.Address, Region.Bytes))
      return std::move(E);
  }
  for (const auto &Fixup : Image->Fixups) {
    if (!Resources->remainingMicroseconds())
      return preparationTimeout(Profile, Options, Image->Architecture,
                                Image->Plan.Entry);
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes, Fixup.Value);
    if (auto E = (*Space)->write(Fixup.Address, Bytes))
      return std::move(E);
  }
  for (const auto &Region : Image->Plan.Regions) {
    if (auto E = (*Space)->protect(Region.Address, Region.Bytes.size(),
                                   Region.Permissions))
      return std::move(E);
  }
  auto Entry =
      (*Space)->canAccess(Image->Plan.Entry, 1, Execute | UserAccessible);
  if (!Entry)
    return Entry.takeError();
  if (!*Entry)
    return failure(diagnostic::ExecutableEntry);
  const auto StackBase = StackTop - Options.StackSize;
  if (auto E =
          MapPages(StackBase, Options.StackSize, Read | Write | UserAccessible))
    return preparationFailure(std::move(E), Profile, Options);
  auto Stack = prepareStack(**Space, Options, Path.filename().string(),
                            Image->Initializers.empty() ? 0 : Image->Plan.Base);
  if (!Stack)
    return Stack.takeError();
  if (auto E = (*Space)->map(ReturnGate, Image->Memory.PageSize, Read | Write))
    return std::move(E);
  const bool X64 = Image->Architecture == GuestArchitecture::X64;
  const uint8_t TrapX64[] = {0x0f, 0x05};
  // svc #0x80; the gate's private address authenticates the main return.
  const uint8_t TrapARM64[] = {0x01, 0x10, 0x00, 0xd4};
  if (auto E =
          (*Space)->write(ReturnGate, X64 ? llvm::ArrayRef<uint8_t>(TrapX64)
                                          : llvm::ArrayRef<uint8_t>(TrapARM64)))
    return std::move(E);
  if (auto E = (*Space)->write(DarwinCallbacks::ReturnGate,
                               X64 ? llvm::ArrayRef<uint8_t>(TrapX64)
                                   : llvm::ArrayRef<uint8_t>(TrapARM64)))
    return std::move(E);
  if (auto E = (*Space)->protect(ReturnGate, Image->Memory.PageSize,
                                 Read | Execute | UserAccessible))
    return std::move(E);
  if (auto E = prepareLibSystem(**Space, *Image))
    return std::move(E);
  auto Backend =
      createExecutionBackend(Options.Backend,
                             X64 ? ExecutionContract::CheckedUserX64
                                 : ExecutionContract::CheckedUserAArch64,
                             *Space, Image->Architecture);
  if (!Backend)
    return Backend.takeError();
  const auto PCRegister = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
  DarwinCallbacks Callbacks(*Image, Options, *Stack);
  auto Start = Callbacks.start(*Backend->CPU);
  if (!Start)
    return Start.takeError();
  if (!Resources->remainingMicroseconds())
    return preparationTimeout(Profile, Options, Image->Architecture,
                              Image->Plan.Entry, &*Backend);
  auto Session = ExecutionSession::create(std::move(Backend->CPU), Resources);
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  DarwinMemory Memory(**Space, Image->Memory, Options);
  DarwinTLS TLS(*Image);
  DarwinFiles Files(CPU, Options.DarwinFiles, Options.OutputLimit,
                    credentialID(ServiceKind::GetEUID, Options.DarwinSystem),
                    Options.DarwinSystem ? Options.DarwinSystem->Credentials
                                         : std::nullopt);
  DarwinEntropy Entropy(Options.DarwinSystem);
  ProcessResult Result{Profile.Profile, Image->Architecture, Backend->Kind,
                       Backend->Reason};
  Result.Entry = Image->Plan.Entry;
  Result.PC = *Start;
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
    auto PC = CPU.readRegister(PCRegister);
    if (!PC) {
      RuntimeFailure(PC.takeError());
      break;
    }
    Result.PC = (*PC)[0];
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
              : diagnostic::MissingContinuation;
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
    auto Import = Image->Imports.find(Request->PC);
    if (Import != Image->Imports.end() &&
        Request->Kind == (X64 ? ServiceRequestKind::X64Syscall
                              : ServiceRequestKind::AArch64SVC) &&
        (X64 || Request->Immediate == LibSystemSVC)) {
      auto Returned = Import->second == LibSystemGateKind::TLVBootstrap
                          ? TLS.invoke(CPU, *Resources, *Request, Result)
                          : invokeLibSystem(CPU, Memory, Files, Entropy, *Image,
                                            Import->second, *Request, Options,
                                            Result, &Callbacks);
      if (!Returned) {
        RuntimeFailure(Returned.takeError());
        break;
      }
      if (!*Returned)
        break;
      continue;
    }
    if (Request->Kind != (X64 ? ServiceRequestKind::X64Syscall
                              : ServiceRequestKind::AArch64SVC) ||
        (!X64 && Request->Immediate != SVCImmediate)) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::ServiceTrap;
      break;
    }
    if (Request->PC == ReturnGate ||
        Request->PC == DarwinCallbacks::ReturnGate) {
      auto Continued = Callbacks.returned(CPU, Request->PC, Result);
      if (!Continued) {
        RuntimeFailure(Continued.takeError());
        break;
      }
      if (!*Continued)
        break;
      continue;
    }
    auto Event = readService(CPU, *Request);
    if (!Event) {
      RuntimeFailure(Event.takeError());
      break;
    }
    Result.Services.push_back(*Event);
    auto Returned =
        handleService(CPU, Memory, Files, Entropy, *Event, Options, Result);
    if (!Returned) {
      RuntimeFailure(Returned.takeError());
      break;
    }
    if (!*Returned)
      break;
    Result.Services.back().Result = (**Returned).Value;
    if ((**Returned).Convention == ServiceConvention::BSD)
      Result.Services.back().Error = (**Returned).Error;
    if (auto E = returnService(CPU, *Request, **Returned)) {
      RuntimeFailure(std::move(E));
      break;
    }
    Result.PC = Request->NextPC;
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  return Result;
}
} // namespace neverd::emulation::darwin_model
