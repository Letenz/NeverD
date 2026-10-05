//===- DarwinTime.cpp - Explicit gettimeofday observations ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Original implementation of the ABI referenced in docs/darwin-emulation.md.
#include "DarwinTime.h"

#include "DarwinUserMemory.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation::darwin_model {
llvm::Error validateTimeOptions(const DarwinTimeOptions &Options) {
  if (Options.TimeOfDay && Options.TimeOfDay->Microseconds >= 1000000)
    return failure(diagnostic::TimeMicroseconds);
  return llvm::Error::success();
}
llvm::Expected<std::optional<ServiceResult>>
timeService(GuestMemory &Memory, const ProcessServiceEvent &Event,
            const std::optional<DarwinTimeOptions> &Options,
            ProcessResult &Result) {
  const auto &A = Event.Arguments;
  auto Unknown = [&](const char *Reason) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Reason;
    return std::optional<ServiceResult>();
  };
  // XNU obtains one calendar/absolute sample before any user copy. Only the
  // requested values are observable, but all requested clock values must exist.
  if (A[0] && (!Options || !Options->TimeOfDay))
    return Unknown(diagnostic::TimeOfDayMissing);
  if (A[2] && (!Options || !Options->MachAbsoluteTime))
    return Unknown(diagnostic::TimeAbsoluteMissing);
  if (A[0]) {
    std::array<uint8_t, 16> Bytes{};
    llvm::support::endian::write64le(Bytes.data(), Options->TimeOfDay->Seconds);
    llvm::support::endian::write32le(Bytes.data() + 8,
                                     Options->TimeOfDay->Microseconds);
    auto Stored = copyUserMemory(Memory, A[0], Bytes,
                                 diagnostic::TimePartialOutput, Result);
    if (!Stored || !*Stored || (**Stored).Error)
      return Stored;
  }
  // Timezone observation and each copy retain the native phase boundary.
  if (A[1]) {
    if (!Options || !Options->Timezone)
      return Unknown(diagnostic::TimeZoneMissing);
    std::array<uint8_t, 8> Bytes;
    llvm::support::endian::write32le(Bytes.data(),
                                     uint32_t(Options->Timezone->MinutesWest));
    llvm::support::endian::write32le(Bytes.data() + 4,
                                     uint32_t(Options->Timezone->DSTTime));
    auto Stored = copyUserMemory(Memory, A[1], Bytes,
                                 diagnostic::TimePartialOutput, Result);
    if (!Stored || !*Stored || (**Stored).Error)
      return Stored;
  }
  if (A[2]) {
    std::array<uint8_t, 8> Bytes;
    llvm::support::endian::write64le(Bytes.data(), *Options->MachAbsoluteTime);
    return copyUserMemory(Memory, A[2], Bytes, diagnostic::TimePartialOutput,
                          Result);
  }
  return std::optional<ServiceResult>({0, false});
}
} // namespace neverd::emulation::darwin_model
