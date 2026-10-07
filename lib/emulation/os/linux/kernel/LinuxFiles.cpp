//===- LinuxFiles.cpp - Typed Linux memory-file service dispatch ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"

namespace neverd::emulation::linux_model {
std::optional<uint64_t> LinuxFiles::unsupported(ProcessResult &Result,
                                                const char *Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason;
  return std::nullopt;
}

std::optional<uint64_t> LinuxFiles::outputError(uint32_t FD) const {
  auto I = Descriptors.find(FD);
  if (I == Descriptors.end())
    return uint64_t(0) - BadDescriptor;
  if (std::holds_alternative<ProcessDescriptor>(I->second))
    return uint64_t(0) - InvalidArgument;
  const auto *S = std::get_if<Stream>(&I->second);
  if (S && (*S == Stream::Output || *S == Stream::Error))
    return std::nullopt;
  return uint64_t(0) - BadDescriptor;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::handle(ServiceKind Kind, const ProcessServiceEvent &Event,
                   ProcessResult &Result) {
  if (!Options)
    return unsupported(Result, FileInputsMissing);
  const auto &[A0, A1, A2, A3, A4, A5] = Event.Arguments;
  switch (Kind) {
  case ServiceKind::Mkdir:
    return makeDirectory(A0, Result);
  case ServiceKind::MkdirAt:
    return makeDirectory(A1, Result);
  case ServiceKind::Access:
    return access(A0, A1, Result);
  case ServiceKind::FaccessAt:
    return access(A1, A2, Result);
  case ServiceKind::Open:
    return open(A0, A1, Result);
  case ServiceKind::OpenAt:
    return open(A1, A2, Result);
  case ServiceKind::Close:
    return close(A0);
  case ServiceKind::Read:
    return readDescriptor(A0, A1, A2, Result);
  case ServiceKind::Lseek:
    return seekDescriptor(A0, A1, A2, Result);
  case ServiceKind::Fstat:
    return statusDescriptor(A0, A1, Result);
  case ServiceKind::FstatAt:
    return statusAt(A0, A1, A2, A3, Result);
  case ServiceKind::StatFS:
    return fileSystemStatus(A0, Result);
  case ServiceKind::FstatFS:
    return fileSystemStatusDescriptor(A0, Result);
  default:
    llvm_unreachable("not a Linux file service");
  }
}
} // namespace neverd::emulation::linux_model
