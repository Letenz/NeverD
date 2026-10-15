//===- AndroidKernelCalls.cpp - Bionic to Linux service boundary ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include <algorithm>

namespace neverd::emulation::android_model {
BionicResult Bionic::kernelCall(std::optional<linux_model::ServiceKind> Kind,
                                const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  const bool IsSyscall = !Kind;
  // API 28 Bionic rejects flags before calling the three-argument syscall.
  if (Kind == linux_model::ServiceKind::FaccessAt && uint32_t(A[3])) {
    if (auto E = setErrno(linux_model::InvalidArgument))
      return std::move(E);
    return value(UINT64_MAX);
  }
  ProcessServiceEvent Event{Call.PC, IsSyscall ? A[0] : 0, {}, std::nullopt};
  // AArch64 Bionic syscall(number, ...) shifts x1..x6 into the six
  // kernel argument registers. The shared Linux table owns the number.
  std::copy_n(A.begin() + (IsSyscall ? 1 : 0), Event.Arguments.size(),
              Event.Arguments.begin());
  // Raw Linux service semantics are shared. Bionic alone owns errno/-1.
  auto *Thread = Threads ? Threads->kernel() : nullptr;
  auto Returned =
      Kind ? Kernel.handle(*Kind, Event, Thread) : Kernel.handle(Event, Thread);
  if (!Returned)
    return Returned.takeError();
  return kernelResult(*Returned);
}

BionicResult Bionic::kernelResult(std::optional<uint64_t> Returned) {
  if (!Returned && Threads && Threads->waitSleep())
    return std::optional<BionicValue>(GuestThreadWait{});
  if (!Returned)
    return std::optional<BionicValue>();
  if (*Returned >= uint64_t(0) - 4095) {
    if (auto E = setErrno(uint64_t(0) - *Returned))
      return std::move(E);
    return value(UINT64_MAX);
  }
  return value(*Returned);
}

BionicResult Bionic::usleep(const NativeCallEvent &Call) {
  // Bionic's useconds_t is unsigned 32-bit, including values >= one second.
  const uint64_t Nanoseconds = uint64_t(uint32_t(Call.Arguments[0])) * 1000;
  const LinuxTimespec Duration{int64_t(Nanoseconds / 1000000000),
                               int64_t(Nanoseconds % 1000000000)};
  return kernelResult(linux_model::sleepRelative(
      Duration, Kernel.clock(), Threads ? Threads->kernel() : nullptr, Result));
}

BionicResult Bionic::syscall(const NativeCallEvent &Call) {
  return kernelCall(std::nullopt, Call);
}

} // namespace neverd::emulation::android_model
