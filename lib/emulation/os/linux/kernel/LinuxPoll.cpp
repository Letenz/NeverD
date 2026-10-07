//===- LinuxPoll.cpp - Observed zero-timeout process-descriptor queries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxFiles.h"
#include "LinuxTime.h"
#include "LinuxUserMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::linux_model {
std::optional<uint16_t> LinuxFiles::pollReadiness(int32_t FD,
                                                  ProcessResult &Result) const {
  if (FD < 0)
    return 0;
  const auto I = Descriptors.find(uint32_t(FD));
  if (I == Descriptors.end())
    return PollInvalidDescriptor;
  // Process descriptors are opened only for known live tasks. Their fixed
  // observation has no exit notification or asynchronous lifetime changes.
  if (std::holds_alternative<ProcessDescriptor>(I->second))
    return 0;
  unsupported(Result, PollDescriptorUnsupported);
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
LinuxFiles::poll(const ProcessServiceEvent &Event,
                 const std::optional<LinuxKernelOptions> &Kernel,
                 ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  if (!Kernel || !Kernel->GKI)
    return unsupported(Result, PollKernelMissing);
  const auto &A = Event.Arguments;
  // The selected subset requires an explicit zero timeout. Import and
  // validate it before signal-mask and descriptor arguments, as the syscall
  // does. Zero timeout never reads a wall clock or writes the timespec back.
  if (!A[2])
    return unsupported(Result, PollWaitUnsupported);
  constexpr uint64_t TimespecSize = 16;
  if (A[2] > Layout.UserLimit || TimespecSize > Layout.UserLimit - A[2])
    return Reply(uint64_t(0) - BadAddress);
  auto Accessible = CPU.canAccess(A[2], TimespecSize, Read | UserAccessible);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return Reply(uint64_t(0) - BadAddress);
  uint8_t TimeBytes[TimespecSize];
  if (auto E = CPU.read(A[2], TimeBytes))
    return std::move(E);
  const LinuxTimespec Timeout{
      int64_t(llvm::support::endian::read64le(TimeBytes)),
      int64_t(llvm::support::endian::read64le(TimeBytes + 8))};
  if (Timeout.Seconds < 0 || !isNormalizedTimespec(Timeout))
    return Reply(uint64_t(0) - InvalidArgument);
  if (A[3]) {
    if (A[4] != SignalMaskSize)
      return Reply(uint64_t(0) - InvalidArgument);
    if (A[3] > Layout.UserLimit || SignalMaskSize > Layout.UserLimit - A[3])
      return Reply(uint64_t(0) - BadAddress);
    auto Mask = CPU.canAccess(A[3], SignalMaskSize, Read | UserAccessible);
    if (!Mask)
      return Mask.takeError();
    if (!*Mask)
      return Reply(uint64_t(0) - BadAddress);
    return unsupported(Result, PollSignalMaskUnsupported);
  }
  if (Timeout.Seconds || Timeout.Nanoseconds)
    return unsupported(Result, PollWaitUnsupported);
  if (!Options)
    return unsupported(Result, PollLimitMissing);
  const uint32_t Count = uint32_t(A[1]);
  if (Count > Options->DescriptorLimit)
    return Reply(uint64_t(0) - InvalidArgument);
  const uint64_t Size = uint64_t(Count) * PollDescriptorSize;
  std::vector<uint8_t> Input(Size);
  if (Count) {
    if (A[0] > Layout.UserLimit || Size > Layout.UserLimit - A[0])
      return Reply(uint64_t(0) - BadAddress);
    auto Mapped = CPU.canAccess(A[0], Size, Read | UserAccessible);
    if (!Mapped)
      return Mapped.takeError();
    if (!*Mapped)
      return Reply(uint64_t(0) - BadAddress);
    if (auto E = CPU.read(A[0], Input))
      return std::move(E);
  }
  std::vector<uint16_t> Revents;
  Revents.reserve(Count);
  uint64_t Ready = 0;
  for (uint32_t I = 0; I != Count; ++I) {
    const int32_t FD = int32_t(
        llvm::support::endian::read32le(Input.data() + I * PollDescriptorSize));
    auto Events = pollReadiness(FD, Result);
    if (!Events)
      return Reply();
    Revents.push_back(*Events);
    Ready += *Events != 0;
  }
  // user_write_access_begin checks the entire user extent even with nfds=0.
  // Permission faults in put_user(revents) still retain earlier entries.
  if (A[0] > Layout.UserLimit || Size > Layout.UserLimit - A[0])
    return Reply(uint64_t(0) - BadAddress);
  for (uint32_t I = 0; I != Count; ++I) {
    uint8_t Bytes[2];
    llvm::support::endian::write16le(Bytes, Revents[I]);
    auto Stored = writeUserMemory(
        CPU, Layout, A[0] + I * PollDescriptorSize + PollReventsOffset, Bytes);
    if (!Stored)
      return Stored.takeError();
    if (*Stored == UserWriteResult::BadAddress)
      return Reply(uint64_t(0) - BadAddress);
    if (*Stored == UserWriteResult::MixedAccess)
      return unsupported(Result, PollPartialOutput);
  }
  return Reply(Ready);
}
} // namespace neverd::emulation::linux_model
