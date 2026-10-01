//===- LinuxServices.cpp - Shared Linux kernel service semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/FormatVariadic.h"
namespace neverd::emulation::linux_model {
namespace {
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

llvm::Expected<std::optional<uint64_t>>
handleService(ExecutionBackend &CPU, LinuxMemory &Memory,
              const ProcessServiceEvent &Event, const ProcessLayout &Layout,
              const ProcessOptions &Options, ProcessResult &Result) {
  auto Kind = serviceKind(Layout.Architecture, Event.Number);
  if (!Kind) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::formatv(Service, Event.Number).str();
    return std::optional<uint64_t>();
  }
  return handleService(CPU, Memory, *Kind, Event, Layout, Options, Result);
}
llvm::Expected<std::optional<uint64_t>>
handleService(ExecutionBackend &CPU, LinuxMemory &Memory, ServiceKind Kind,
              const ProcessServiceEvent &Event, const ProcessLayout &Layout,
              const ProcessOptions &Options, ProcessResult &Result) {
  switch (Kind) {
  case ServiceKind::Exit:
  case ServiceKind::ExitGroup:
    Result.Stop = ProcessStopReason::Exited;
    Result.ExitStatus = Event.Arguments[0] & ExitMask;
    return std::optional<uint64_t>();
  case ServiceKind::GetPID:
    return std::optional<uint64_t>(ProcessID);
  case ServiceKind::GetTID:
    return std::optional<uint64_t>(ThreadID);
  case ServiceKind::Mmap:
  case ServiceKind::Mprotect:
  case ServiceKind::Munmap:
  case ServiceKind::Brk:
    return Memory.handle(Kind, Event, Result);
  case ServiceKind::ArchPrctl:
    return archPrctl(CPU, Event, Layout, Result);
  case ServiceKind::Write:
    return writeOutput(CPU, Event, Layout, Options, Result);
  }
  llvm_unreachable("unhandled Linux service kind");
}
} // namespace neverd::emulation::linux_model
