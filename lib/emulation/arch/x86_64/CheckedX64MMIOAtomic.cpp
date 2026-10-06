//===- CheckedX64MMIOAtomic.cpp - Native results, atomic device effects ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/MMIOAtomicTransaction.h"
#include "CheckedX64Backend.h"
#include "X64Exception.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::emulation {
llvm::Error CheckedX64Backend::deviceAtomic(const cs_insn &I, uint64_t Address,
                                            unsigned Size) {
  if (UserMode || Address % Size)
    return llvm::make_error<UnsupportedExecutionError>();
  if (Hooks.Read)
    Hooks.Read(Address, Size);
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  if (auto E = access(Address, Size, Read | Write, true, true))
    return E;
  if (StopRequested || FirstFault)
    return llvm::Error::success();
  auto T = MMIOAtomicTransaction::prepare(
      *Memory, Address, Size, {Deadline, &StopRequested}, DeviceFailed);
  if (!T)
    return T.takeError();
  if (auto E = Memory->stageDeviceOperand(Address, x64::DeviceOperandGPA,
                                          (*T)->original()))
    return E;
  auto Retire = llvm::scope_exit([&] { Memory->retireDeviceOperand(); });
  auto Root =
      buildX64PageTables(*Memory, false, Machine->requiresExceptionMonitor());
  if (!Root)
    return Root.takeError();
  if (auto E = Memory->prepareTransportRead(I.address, I.size))
    return E;
  auto Next = CPU;
  if (auto E = Machine->step(Next, *Root, {Deadline, &StopRequested}))
    return llvm::handleErrors(std::move(E), [&](const X64ExceptionError &E) {
      CPU = Next;
      BackendFault Fault{BackendFaultKind::Interrupt, I.address};
      Fault.Interrupt = E.exception().Vector;
      Fault.Address = E.exception().FaultAddress;
      Fault.ErrorCode = E.exception().ErrorCode;
      return raiseFault(Fault, true);
    });
  if (FirstFault)
    return llvm::Error::success();
  const auto Result = llvm::ArrayRef(
      Memory->data() + x64::DeviceOperandGPA + Address % x64::PageSize, Size);
  auto Committed =
      (*T)->commit(Result, Hooks, [&] { return bool(FirstFault); });
  if (!Committed)
    return Committed.takeError();
  if (*Committed)
    CPU = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
