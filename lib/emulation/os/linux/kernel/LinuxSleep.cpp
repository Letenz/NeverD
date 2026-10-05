//===- LinuxSleep.cpp - Relative nanosleep admission ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
sleepService(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
             const MemoryLayout &Layout, LinuxClock &Clock,
             ThreadContext *Thread, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (!Clock.advancesOnIdle()) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = SleepPolicy;
    return Reply();
  }
  // Linux copies both LP64 fields before validating either value. Failed
  // input copies have no observable partial output to the guest.
  uint8_t Bytes[16];
  const uint64_t Address = Event.Arguments[0];
  if (Address > Layout.UserLimit || sizeof(Bytes) > Layout.UserLimit - Address)
    return Reply(0 - BadAddress);
  auto Readable = CPU.canAccess(Address, sizeof(Bytes), Read | UserAccessible);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return Reply(0 - BadAddress);
  if (auto E = CPU.read(Address, Bytes))
    return std::move(E);
  const LinuxTimespec Duration{
      int64_t(llvm::support::endian::read64le(Bytes)),
      int64_t(llvm::support::endian::read64le(Bytes + 8))};
  if (Duration.Seconds < 0 || !isNormalizedTimespec(Duration))
    return Reply(0 - InvalidArgument);
  auto Deadline = Clock.deadline(Duration, Result);
  if (!Deadline)
    return Reply();
  // A successful relative sleep never touches the remaining-time pointer,
  // even when it aliases the request or is inaccessible. Signals and restart
  // delivery have no implementation in this explicit workload policy.
  if (*Deadline == Clock.elapsed())
    return Reply(0);
  if (Thread) {
    assert(!Thread->SleepDeadline && !Thread->Exit);
    Thread->SleepDeadline = *Deadline;
    return Reply();
  }
  if (!Clock.advanceTo(*Deadline, Result))
    return Reply();
  return Reply(0);
}
} // namespace neverd::emulation::linux_model
