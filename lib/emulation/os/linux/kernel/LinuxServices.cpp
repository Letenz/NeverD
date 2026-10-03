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
#include "../LinuxValues.def"
#undef NEVERD_LINUX_SERVICE
#define NEVERD_LINUX_X64_SERVICE(Name, Value, Count)                           \
  if (ISA == GuestArchitecture::X64 && Number == Value)                        \
    return ServiceKind::Name;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_X64_SERVICE
  return std::nullopt;
}
} // namespace

llvm::Expected<std::optional<uint64_t>>
handleService(ExecutionBackend &CPU, LinuxMemory &Memory,
              const ProcessServiceEvent &Event, const MemoryLayout &Layout,
              const ProcessOptions &Options, ProcessResult &Result) {
  auto Kind = serviceKind(CPU.architecture(), Event.Number);
  if (!Kind) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::formatv(Service, Event.Number).str();
    return std::optional<uint64_t>();
  }
  return handleService(CPU, Memory, *Kind, Event, Layout, Options, Result);
}
llvm::Expected<std::optional<uint64_t>>
handleService(ExecutionBackend &CPU, LinuxMemory &Memory, ServiceKind Kind,
              const ProcessServiceEvent &Event, const MemoryLayout &Layout,
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
  case ServiceKind::GetUID:
  case ServiceKind::GetEUID:
    return std::optional<uint64_t>(UserID);
  case ServiceKind::GetGID:
  case ServiceKind::GetEGID:
    return std::optional<uint64_t>(GroupID);
  case ServiceKind::Mmap:
  case ServiceKind::Mprotect:
  case ServiceKind::Munmap:
  case ServiceKind::Brk:
    return Memory.handle(Kind, Event, Result);
  case ServiceKind::ArchPrctl:
    return archPrctl(CPU, Event, Layout, Result);
  case ServiceKind::Write:
  case ServiceKind::WriteV:
    return writeOutput(CPU, Kind, Event, Layout, Options, Result);
  }
  llvm_unreachable("unhandled Linux service kind");
}
} // namespace neverd::emulation::linux_model
