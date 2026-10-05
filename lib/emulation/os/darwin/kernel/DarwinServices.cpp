//===- DarwinServices.cpp - Darwin BSD service ABI and effects ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFiles.h"
#include "DarwinMemory.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
using enum CPURegister;
constexpr CPURegister X64Arguments[] = {X64DI,  X64SI, X64DX,
                                        X64R10, X64R8, X64R9};
constexpr CPURegister ARMArguments[] = {AArch64X0, AArch64X1, AArch64X2,
                                        AArch64X3, AArch64X4, AArch64X5};
std::optional<ServiceKind> serviceKind(GuestArchitecture ISA, uint64_t Number) {
  if (ISA == GuestArchitecture::X64) {
    if ((Number & ~uint64_t(0x00ffffff)) != BSDClass)
      return std::nullopt;
    Number -= BSDClass;
  }
  struct Binding {
    uint64_t Number;
    ServiceKind Kind;
  };
  static constexpr Binding Bindings[] = {
#define NEVERD_DARWIN_SERVICE(Name, Code, ReturnType) {Code, ServiceKind::Name},
#define NEVERD_DARWIN_SERVICE_ALIAS(Name, Code) {Code, ServiceKind::Name},
#include "../DarwinValues.def"
#undef NEVERD_DARWIN_SERVICE_ALIAS
#undef NEVERD_DARWIN_SERVICE
  };
  for (const auto &Entry : Bindings)
    if (Entry.Number == Number)
      return Entry.Kind;
  return std::nullopt;
}
llvm::Expected<std::optional<ServiceResult>>
writeOutput(ExecutionBackend &CPU, DarwinFiles &Files,
            const ProcessServiceEvent &Event, const ProcessOptions &Options,
            ProcessResult &Result) {
  const uint32_t FD = Event.Arguments[0];
  const uint64_t Address = Event.Arguments[1], Count = Event.Arguments[2];
  // XNU write_internal validates nbyte before descriptor lookup or copying.
  // An invalid request cannot consume the output allowance or publish bytes.
  if (Count > MaxWriteBytes)
    return std::optional<ServiceResult>({InvalidArgument, true});
  auto Sink = Files.outputSink(FD);
  if (!Sink)
    return std::optional<ServiceResult>({BadDescriptor, true});
  if (!Count)
    return std::optional<ServiceResult>({0, false});
  if (Address >= UserLimit || Count > UserLimit - Address)
    return std::optional<ServiceResult>({BadAddress, true});
  const uint64_t Used =
      Result.StandardOutput.size() + Result.StandardError.size();
  if (Used > Options.OutputLimit || Count > Options.OutputLimit - Used) {
    Result.Stop = ProcessStopReason::OutputLimit;
    Result.Diagnostic = diagnostic::OutputLimit;
    return std::optional<ServiceResult>();
  }
  uint64_t Readable = 0;
  // Access granules remain the CPU's 4 KiB mappings. Querying permissions must
  // not publish a CPU fault while delivering a recoverable BSD errno.
  while (Readable < Count) {
    const uint64_t Start = Address + Readable;
    const uint64_t Size = std::min(Count - Readable, 4096 - Start % 4096);
    auto Access = CPU.canAccess(Start, Size, Read | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      break;
    Readable += Size;
  }
  if (!Readable)
    return std::optional<ServiceResult>({BadAddress, true});
  std::string Bytes(Readable, '\0');
  if (auto E = CPU.read(Address, llvm::MutableArrayRef<uint8_t>(
                                     reinterpret_cast<uint8_t *>(Bytes.data()),
                                     Bytes.size())))
    return std::move(E);
  (*Sink == 1 ? Result.StandardOutput : Result.StandardError).append(Bytes);
  // XNU's write path retains EFAULT after a partial copy; unlike interruption
  // errors, it does not convert that error into a successful short write.
  return std::optional<ServiceResult>(Readable == Count
                                          ? ServiceResult{Readable, false}
                                          : ServiceResult{BadAddress, true});
}
} // namespace

llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  auto Number = CPU.readRegister(X64 ? X64AX : AArch64X16);
  if (!Number)
    return Number.takeError();
  ProcessServiceEvent Event{Request.PC, (*Number)[0], {}, std::nullopt};
  for (size_t I = 0; I < Event.Arguments.size(); ++I) {
    auto Value = CPU.readRegister(X64 ? X64Arguments[I] : ARMArguments[I]);
    if (!Value)
      return Value.takeError();
    Event.Arguments[I] = (*Value)[0];
  }
  return Event;
}

llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          ServiceResult Result) {
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  const auto FlagRegister = X64 ? X64FLAGS : AArch64NZCV;
  auto Flags = CPU.readRegister(FlagRegister);
  if (!Flags)
    return Flags.takeError();
  const uint64_t Carry = X64 ? 1 : CarryARM64;
  (*Flags)[0] = ((*Flags)[0] & ~Carry) | (Result.Error ? Carry : 0);
  if (auto E = CPU.writeRegister(FlagRegister, *Flags))
    return E;
  if (auto E = CPU.writeRegister(X64 ? X64AX : AArch64X0, {Result.Value, 0}))
    return E;
  // XNU preserves RDX on x64 error; ARM64 clears X1 on both paths.
  if (!X64 || !Result.Error)
    if (auto E = CPU.writeRegister(X64 ? X64DX : AArch64X1, {0, 0}))
      return E;
  if (X64) {
    if (auto E = CPU.writeRegister(X64CX, {Request.NextPC, 0}))
      return E;
    if (auto E = CPU.writeRegister(X64R11, *Flags))
      return E;
  }
  return CPU.writeRegister(X64 ? X64PC : AArch64PC, {Request.NextPC, 0});
}

llvm::Expected<std::optional<ServiceResult>>
handleService(ExecutionBackend &CPU, DarwinMemory &Memory, DarwinFiles &Files,
              const ProcessServiceEvent &Event, const ProcessOptions &Options,
              ProcessResult &Result) {
  auto Kind = serviceKind(CPU.architecture(), Event.Number);
  if (!Kind) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        llvm::formatv(diagnostic::UnsupportedService, Event.Number).str();
    return std::optional<ServiceResult>();
  }
  switch (*Kind) {
  case ServiceKind::Exit:
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = Event.Arguments[0] & 0xff;
    return std::optional<ServiceResult>();
  case ServiceKind::Write:
    return writeOutput(CPU, Files, Event, Options, Result);
  case ServiceKind::Read:
  case ServiceKind::Pread:
  case ServiceKind::Open:
  case ServiceKind::Close:
  case ServiceKind::Lseek:
  case ServiceKind::Dup:
  case ServiceKind::Dup2:
  case ServiceKind::Fcntl:
  case ServiceKind::Stat64:
  case ServiceKind::Fstat64:
  case ServiceKind::Lstat64:
    return Files.handle(*Kind, Event, Result);
  case ServiceKind::GetPID:
    return std::optional<ServiceResult>({ProcessID, false});
  case ServiceKind::GetPPID:
    return std::optional<ServiceResult>({ParentID, false});
  case ServiceKind::GetUID:
  case ServiceKind::GetEUID:
    return std::optional<ServiceResult>({UserID, false});
  case ServiceKind::GetGID:
  case ServiceKind::GetEGID:
    return std::optional<ServiceResult>({GroupID, false});
  case ServiceKind::Mmap:
  case ServiceKind::Mprotect:
  case ServiceKind::Munmap:
    return Memory.handle(*Kind, Event, Result);
  }
  llvm_unreachable("unknown Darwin service");
}
} // namespace neverd::emulation::darwin_model
