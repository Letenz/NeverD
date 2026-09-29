//===- LinuxThreadPointer.cpp - Guest arch_prctl thread state ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxProcess.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
archPrctl(ExecutionBackend &CPU, const ProcessServiceEvent &Event,
          const ProcessLayout &Layout, ProcessResult &Result) {
  CPURegister Register;
  bool Set;
  switch (Event.Arguments[0]) {
#define NEVERD_LINUX_THREAD_POINTER(Name, Number, Identity, Write)             \
  case Number:                                                                 \
    Register = CPURegister::Identity;                                          \
    Set = Write;                                                               \
    break;
#include "LinuxValues.def"
#undef NEVERD_LINUX_THREAD_POINTER
  default:
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic =
        llvm::formatv(ThreadOperation, Event.Arguments[0]).str();
    return std::optional<uint64_t>();
  }
  const uint64_t Address = Event.Arguments[1];
  if (Set) {
    // Linux accepts an unmapped base, but never a kernel-range address. A
    // later TLS load remains subject to ordinary user memory permissions.
    if (Address >= Layout.UserLimit)
      return std::optional<uint64_t>(uint64_t(0) - PermissionDenied);
    if (auto E = CPU.writeRegister(Register, {Address, 0}))
      return std::move(E);
  } else {
    const auto Width = Layout.Calls.info().WordSize;
    if (Address >= Layout.UserLimit || Width > Layout.UserLimit - Address)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Writable = CPU.canAccess(Address, Width, Write | UserAccessible);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return std::optional<uint64_t>(uint64_t(0) - BadAddress);
    auto Value = CPU.readRegister(Register);
    if (!Value)
      return Value.takeError();
    if (auto E = CPU.writeInteger(Address, (*Value)[0], Width))
      return std::move(E);
  }
  return std::optional<uint64_t>(0);
}
} // namespace neverd::emulation::linux_model
