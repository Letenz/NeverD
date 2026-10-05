//===- LinuxTime.cpp - Fixed guest clocks and Linux time services --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"

#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
namespace {
bool knownClock(int32_t ID) {
  switch (ID) {
#define NEVERD_LINUX_CLOCK(Name, Number)                                       \
  case Number:                                                                 \
    return true;
#include "../LinuxValues.def"
#undef NEVERD_LINUX_CLOCK
  default:
    return false;
  }
}
using Reply = std::optional<uint64_t>;
Reply unsupported(ProcessResult &Result, llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
  return std::nullopt;
}
llvm::Expected<Reply> put(ExecutionBackend &CPU, uint64_t Address,
                          llvm::ArrayRef<uint8_t> Bytes,
                          const MemoryLayout &Layout, ProcessResult &Result) {
  auto Stored = writeUserMemory(CPU, Layout, Address, Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return Reply(0);
  case UserWriteResult::BadAddress:
    return Reply(0 - BadAddress);
  case UserWriteResult::MixedAccess:
    return unsupported(Result, TimePartialOutput);
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}
llvm::Expected<Reply> putWord(ExecutionBackend &CPU, uint64_t Address,
                              int64_t Word, const MemoryLayout &Layout,
                              ProcessResult &Result) {
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, static_cast<uint64_t>(Word));
  return put(CPU, Address, Bytes, Layout, Result);
}
} // namespace

bool isNormalizedTimespec(const LinuxTimespec &Value) {
  return Value.Nanoseconds >= 0 && Value.Nanoseconds < 1000000000;
}
llvm::Error validateTimeOptions(const LinuxTimeOptions &Options) {
  for (const auto &[ID, Value] : Options.Clocks) {
    if (!knownClock(ID))
      return failure(TimeClockOption);
    if (!isNormalizedTimespec(Value))
      return failure(TimeNanoseconds);
  }
  return llvm::Error::success();
}
std::optional<LinuxTimespec>
clockValue(int32_t ID, const ProcessOptions &Options, ProcessResult &Result) {
  if (Options.LinuxTime) {
    auto I = Options.LinuxTime->Clocks.find(ID);
    if (I != Options.LinuxTime->Clocks.end())
      return I->second;
  }
  unsupported(Result, llvm::formatv(TimeClockMissing, ID).str());
  return std::nullopt;
}
llvm::Expected<Reply> timeService(ExecutionBackend &CPU, ServiceKind Kind,
                                  const ProcessServiceEvent &Event,
                                  const MemoryLayout &Layout,
                                  const ProcessOptions &Options,
                                  ProcessResult &Result) {
  const auto &A = Event.Arguments;
  if (Kind == ServiceKind::ClockGetTime) {
    // clockid_t is a signed 32-bit ABI argument, including on LP64.
    int32_t ID = static_cast<int32_t>(static_cast<uint32_t>(A[0]));
    if (ID < 0)
      return unsupported(Result, TimeDynamicClock);
    if (!knownClock(ID))
      return Reply(0 - InvalidArgument);
    auto Value = clockValue(ID, Options, Result);
    if (!Value)
      return Reply();
    uint8_t Bytes[16];
    llvm::support::endian::write64le(Bytes,
                                     static_cast<uint64_t>(Value->Seconds));
    llvm::support::endian::write64le(Bytes + 8, Value->Nanoseconds);
    return put(CPU, A[1], Bytes, Layout, Result);
  }
  if (Kind == ServiceKind::Time) {
    auto Value = clockValue(ClockRealtime, Options, Result);
    if (!Value)
      return Reply();
    if (A[0]) {
      auto Stored = putWord(CPU, A[0], Value->Seconds, Layout, Result);
      if (!Stored || !*Stored || **Stored)
        return Stored;
    }
    return Reply(static_cast<uint64_t>(Value->Seconds));
  }
  assert(Kind == ServiceKind::GetTimeOfDay);
  if (A[0]) {
    auto Value = clockValue(ClockRealtime, Options, Result);
    if (!Value)
      return Reply();
    // Linux gettimeofday performs two ordered put_user operations before tz.
    auto Stored = putWord(CPU, A[0], Value->Seconds, Layout, Result);
    if (!Stored || !*Stored || **Stored)
      return Stored;
    Stored = putWord(CPU, A[0] + 8, Value->Nanoseconds / 1000, Layout, Result);
    if (!Stored || !*Stored || **Stored)
      return Stored;
  }
  if (A[1]) {
    if (!Options.LinuxTime || !Options.LinuxTime->Timezone)
      return unsupported(Result, TimezoneMissing);
    const auto &Zone = *Options.LinuxTime->Timezone;
    uint8_t Bytes[8];
    llvm::support::endian::write32le(Bytes,
                                     static_cast<uint32_t>(Zone.MinutesWest));
    llvm::support::endian::write32le(Bytes + 4,
                                     static_cast<uint32_t>(Zone.DSTTime));
    return put(CPU, A[1], Bytes, Layout, Result);
  }
  return Reply(0);
}
} // namespace neverd::emulation::linux_model
