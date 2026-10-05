//===- LinuxSignals.cpp - Explicit LP64 signal action state --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxSignals.h"

#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
namespace {
constexpr uint64_t Unblockable =
    (uint64_t(1) << (SignalKill - 1)) | (uint64_t(1) << (SignalStop - 1));
// Common LP64 Linux flags. Later flag-probing extensions need their own ABI.
constexpr uint64_t ActionFlags = 0xdc000007;
bool validSignal(int32_t Signal) {
  return Signal > 0 && uint32_t(Signal) <= SignalCount;
}
bool kernelOnly(int32_t Signal) {
  return Signal == SignalKill || Signal == SignalStop;
}
void unsupported(ProcessResult &Result, llvm::StringRef Reason) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Reason.str();
}
bool supportedFlags(uint64_t Flags) {
  if (uint32_t(Flags) & ~ActionFlags)
    return false;
  // Bionic's signed int flags become unsigned long in the kernel structure.
  return Flags <= UINT32_MAX ||
         Flags == uint64_t(int64_t(int32_t(uint32_t(Flags))));
}
} // namespace

llvm::Error validateSignalOptions(const LinuxSignalOptions &Options) {
  for (const auto &[Signal, Action] : Options.Actions) {
    if (!validSignal(Signal) || (Action.Mask & Unblockable))
      return failure(SignalOptions);
    if (kernelOnly(Signal) &&
        (Action.Handler || Action.Flags || Action.Restorer || Action.Mask))
      return failure(SignalOptions);
  }
  return llvm::Error::success();
}

std::optional<SignalActionResult>
LinuxSignals::action(int32_t Signal, const LinuxSignalAction *NewAction,
                     bool ReadOld, ProcessResult &Result) {
  if (!validSignal(Signal) || (NewAction && kernelOnly(Signal)))
    return SignalActionResult{uint64_t(0) - InvalidArgument, {}};
  if (!NewAction && !ReadOld)
    return SignalActionResult{};
  if (!Enabled) {
    unsupported(Result, SignalInputMissing);
    return std::nullopt;
  }
  if (NewAction && !supportedFlags(NewAction->Flags)) {
    unsupported(Result, SignalFlagsUnsupported);
    return std::nullopt;
  }
  SignalActionResult Reply;
  if (ReadOld) {
    auto I = State.Actions.find(Signal);
    if (I == State.Actions.end()) {
      unsupported(Result, llvm::formatv(SignalActionMissing, Signal).str());
      return std::nullopt;
    }
    Reply.Previous = I->second;
  }
  if (NewAction) {
    auto Stored = *NewAction;
    Stored.Mask &= ~Unblockable;
    State.Actions[Signal] = Stored;
  }
  return Reply;
}

llvm::Expected<std::optional<uint64_t>>
LinuxSignals::handle(ExecutionBackend &CPU, const MemoryLayout &Layout,
                     const ProcessServiceEvent &Event, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  const auto &A = Event.Arguments;
  if (A[3] != SignalMaskSize)
    return Reply(uint64_t(0) - InvalidArgument);
  LinuxSignalAction NewAction;
  if (A[1]) {
    if (A[1] > Layout.UserLimit || SignalActionSize > Layout.UserLimit - A[1])
      return Reply(uint64_t(0) - BadAddress);
    auto Readable =
        CPU.canAccess(A[1], SignalActionSize, Read | UserAccessible);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return Reply(uint64_t(0) - BadAddress);
    uint8_t Bytes[SignalActionSize];
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    NewAction = {llvm::support::endian::read64le(Bytes),
                 llvm::support::endian::read64le(Bytes + 8),
                 llvm::support::endian::read64le(Bytes + 16),
                 llvm::support::endian::read64le(Bytes + 24)};
  }
  auto Applied = action(int32_t(uint32_t(A[0])), A[1] ? &NewAction : nullptr,
                        A[2] != 0, Result);
  if (!Applied)
    return Reply();
  if (Applied->Status || !A[2])
    return Reply(Applied->Status);
  uint8_t Bytes[SignalActionSize];
  const auto &Old = Applied->Previous;
  llvm::support::endian::write64le(Bytes, Old.Handler);
  llvm::support::endian::write64le(Bytes + 8, Old.Flags);
  llvm::support::endian::write64le(Bytes + 16, Old.Restorer);
  llvm::support::endian::write64le(Bytes + 24, Old.Mask);
  // The new disposition is already installed when the old-action copy fails.
  auto Stored = writeUserMemory(CPU, Layout, A[2], Bytes);
  if (!Stored)
    return Stored.takeError();
  switch (*Stored) {
  case UserWriteResult::Stored:
    return Reply(0);
  case UserWriteResult::BadAddress:
    return Reply(uint64_t(0) - BadAddress);
  case UserWriteResult::MixedAccess:
    unsupported(Result, SignalPartialOutput);
    return Reply();
  }
  llvm_unreachable("unknown Linux user-copy outcome");
}
} // namespace neverd::emulation::linux_model
