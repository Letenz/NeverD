//===- MMIOAtomicTransaction.cpp - Preview, observe and commit once -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MMIOAtomicTransaction.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"

#include "llvm/Support/MathExtras.h"

#include <climits>

namespace neverd::emulation {
namespace {
template <typename F> auto deviceCall(bool &Failed, F Call) {
  try {
    auto Result = Call();
    // Error and Expected have opposite truth values, so the caller checks
    // returned errors. Exceptions are classified at this callback boundary.
    return Result;
  } catch (...) {
    Failed = true;
    throw;
  }
}
llvm::Error deviceError(bool &Failed, llvm::Error E) {
  if (E)
    Failed = true;
  return E;
}
} // namespace

MMIOAtomicTransaction::MMIOAtomicTransaction(
    std::unique_lock<std::recursive_mutex> Lease, uint64_t Address,
    GuestMMIOPreparedAtomic Prepared, MachineRunControl Control,
    bool &DeviceFailed)
    : Lease(std::move(Lease)), Address(Address), Prepared(std::move(Prepared)),
      Control(Control), DeviceFailed(DeviceFailed) {}

llvm::Expected<std::unique_ptr<MMIOAtomicTransaction>>
MMIOAtomicTransaction::prepare(MemoryProjection &Memory, uint64_t Address,
                               unsigned Size, MachineRunControl Control,
                               bool &DeviceFailed) {
  if (auto E = Memory.prepareWrite())
    return E;
  auto Lease = Memory.executionLock();
  if (!Lease)
    return Lease.takeError();
  const auto P = Memory.mappings().find(Address & ~(memory::PageSize - 1));
  if (!llvm::isPowerOf2_32(Size) || Size > execution_limits::MMIOAtomicBytes ||
      Address % Size || P == Memory.mappings().end() || !P->second.IO ||
      (P->second.Permissions & (Read | Write)) != (Read | Write))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto D = P->second.IO;
  if (Address < D->Address || Address - D->Address >= D->Size ||
      Size > D->Size - (Address - D->Address) || !D->Callbacks.PrepareAtomic)
    return llvm::make_error<UnsupportedExecutionError>();
  const auto Offset = Address - D->Address;
  for (bool Write : {false, true}) {
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::DeviceAtomicInterrupted,
                                     Control);
    if (auto E = deviceCall(DeviceFailed, [&] {
          return D->Callbacks.Validate(Offset, Size, Write);
        }))
      return deviceError(DeviceFailed, std::move(E));
  }
  if (Control.interrupted())
    return diagnostic::interrupted(diagnostic::DeviceAtomicInterrupted,
                                   Control);
  auto Prepared = deviceCall(
      DeviceFailed, [&] { return D->Callbacks.PrepareAtomic(Offset, Size); });
  if (!Prepared)
    return deviceError(DeviceFailed, Prepared.takeError());
  if (Prepared->Value.size() != Size || !Prepared->Commit)
    return deviceError(DeviceFailed,
                       diagnostic::error(diagnostic::DeviceAtomic));
  if (Control.interrupted())
    return diagnostic::interrupted(diagnostic::DeviceAtomicInterrupted,
                                   Control);
  return std::unique_ptr<MMIOAtomicTransaction>(new MMIOAtomicTransaction(
      std::move(*Lease), Address, std::move(*Prepared), Control, DeviceFailed));
}

llvm::Expected<bool>
MMIOAtomicTransaction::commit(llvm::ArrayRef<uint8_t> Result,
                              const BackendHooks &Hooks,
                              llvm::function_ref<bool()> Rejected) {
  if (Attempted || Result.size() != Prepared.Value.size())
    return diagnostic::error(diagnostic::DeviceAtomic);
  Attempted = true;
  for (unsigned Offset = 0; Offset < Result.size();
       Offset += execution_limits::MMIOObserverBytes) {
    if (Control.interrupted())
      return diagnostic::interrupted(diagnostic::DeviceAtomicInterrupted,
                                     Control);
    const auto Bytes = Result.drop_front(Offset).take_front(
        execution_limits::MMIOObserverBytes);
    uint64_t Value = 0;
    for (unsigned N = 0; N < Bytes.size(); ++N)
      Value |= uint64_t(Bytes[N]) << (N * CHAR_BIT);
    if (Hooks.Write)
      Hooks.Write(Address + Offset, Bytes.size(), Value);
    if (Rejected())
      return false;
  }
  if (Control.interrupted())
    return diagnostic::interrupted(diagnostic::DeviceAtomicInterrupted,
                                   Control);
  if (Rejected())
    return false;
  if (auto E = deviceError(DeviceFailed, deviceCall(DeviceFailed, [&] {
                             return Prepared.Commit(Result);
                           })))
    return std::move(E);
  return true;
}
} // namespace neverd::emulation
