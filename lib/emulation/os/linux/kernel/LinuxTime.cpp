//===- LinuxTime.cpp - Fixed guest clocks and Linux time services --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTime.h"

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
// A kernel put_user/copy_to_user may fault after an architecture-dependent
// partial store. Model complete writes and wholly inaccessible destinations;
// stop before a mixed-access operation rather than inventing its fault bytes.
llvm::Expected<Reply> put(ExecutionBackend &CPU, uint64_t Address,
                          llvm::ArrayRef<uint8_t> Bytes,
                          const MemoryLayout &Layout, ProcessResult &Result) {
  if (Address >= Layout.UserLimit || Bytes.size() > Layout.UserLimit - Address)
    return Reply(0 - BadAddress);
  auto Writable = CPU.canAccess(Address, Bytes.size(), Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable) {
    for (size_t I = 0; I < Bytes.size(); ++I) {
      auto Part = CPU.canAccess(Address + I, 1, Write | UserAccessible);
      if (!Part)
        return Part.takeError();
      if (*Part)
        return unsupported(Result,
                           "partial Linux time output fault is unmodeled");
    }
    return Reply(0 - BadAddress);
  }
  if (auto E = CPU.write(Address, Bytes))
    return std::move(E);
  return Reply(0);
}
llvm::Expected<Reply> putWord(ExecutionBackend &CPU, uint64_t Address,
                              int64_t Word, const MemoryLayout &Layout,
                              ProcessResult &Result) {
  uint8_t Bytes[8];
  llvm::support::endian::write64le(Bytes, static_cast<uint64_t>(Word));
  return put(CPU, Address, Bytes, Layout, Result);
}
} // namespace

llvm::Error validateTimeOptions(const LinuxTimeOptions &Options) {
  for (const auto &[ID, Value] : Options.Clocks) {
    if (!knownClock(ID))
      return failure("linux_time requires a known static Linux clock ID");
    if (Value.Nanoseconds < 0 || Value.Nanoseconds >= 1000000000)
      return failure("linux_time nanoseconds must be in [0, 1000000000)");
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
  unsupported(
      Result,
      llvm::formatv("Linux clock {0} has no explicit linux_time input", ID)
          .str());
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
      return unsupported(Result,
                         "dynamic and encoded Linux clocks are unmodeled");
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
      return unsupported(Result,
                         "Linux timezone has no explicit linux_time input");
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
