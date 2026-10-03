//===- LinuxServiceABI.cpp - Linux user service register contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxKernel.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation::linux_model {
namespace {
using enum CPURegister;
#define NEVERD_LINUX_ABI(ISA, Trap, Number, Result, SP, PC, ...)               \
  constexpr ServiceABI ISA##ABI{                                               \
      ServiceRequestKind::Trap, Number, Result, SP, PC, {__VA_ARGS__}};
#include "../LinuxValues.def"
#undef NEVERD_LINUX_ABI
} // namespace
const ServiceABI &serviceABI(GuestArchitecture Architecture) {
  return Architecture == GuestArchitecture::X64 ? X64ABI : AArch64ABI;
}
llvm::Expected<ProcessServiceEvent> readService(ExecutionBackend &CPU,
                                                const ServiceRequest &Request) {
  const auto &ABI = serviceABI(CPU.architecture());
  if (Request.Kind != ABI.Trap)
    return failure(Trap);
  auto Number = CPU.readRegister(ABI.Number);
  if (!Number)
    return Number.takeError();
  ProcessServiceEvent Event{Request.PC, (*Number)[0], {}, std::nullopt};
  for (size_t I = 0; I < ABI.Arguments.size(); ++I) {
    auto Value = CPU.readRegister(ABI.Arguments[I]);
    if (!Value)
      return Value.takeError();
    Event.Arguments[I] = (*Value)[0];
  }
  return Event;
}
llvm::Error returnService(ExecutionBackend &CPU, const ServiceRequest &Request,
                          uint64_t Result) {
  const auto &ABI = serviceABI(CPU.architecture());
  if (Request.Kind != ABI.Trap)
    return failure(Trap);
  // SYSCALL was intercepted before architectural entry. Supply its user-
  // visible clobbers when the Linux model completes a returning service.
  if (CPU.architecture() == GuestArchitecture::X64) {
    auto Flags = CPU.reg(X64Register::FLAGS);
    if (!Flags)
      return Flags.takeError();
    if (auto E = CPU.setReg(X64Register::CX, Request.NextPC))
      return E;
    if (auto E = CPU.setReg(X64Register::R11, *Flags))
      return E;
  }
  if (auto E = CPU.writeRegister(ABI.Result, {Result, 0}))
    return E;
  return CPU.writeRegister(ABI.PC, {Request.NextPC, 0});
}
} // namespace neverd::emulation::linux_model
