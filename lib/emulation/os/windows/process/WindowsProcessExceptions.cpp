//===- WindowsProcessExceptions.cpp - Guest vectored exception dispatch --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessExceptions.h"

#include "WindowsProcessContext.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::windows_process {
using namespace value;
static_assert(ExceptionArgumentsOffset + MaxExceptionArguments * PointerSize ==
              ExceptionRecordSize);
void VectoredExceptions::collect() {
  if (Frames.empty())
    Handlers.remove_if([](const auto &H) { return !H.Live; });
}
llvm::Expected<uint64_t> VectoredExceptions::add(bool First, uint64_t PC) {
  collect();
  if (Handlers.size() >= MaxExceptionHandlers || NextHandle >= UserLimit)
    return failure(text::ExceptionLimit);
  Handler H{NextHandle, PC};
  NextHandle += PointerSize;
  if (First)
    Handlers.push_front(H);
  else
    Handlers.push_back(H);
  return H.Handle;
}
uint64_t VectoredExceptions::remove(uint64_t Handle) {
  for (auto &H : Handlers)
    if (H.Handle == Handle && H.Live) {
      H.Live = false;
      // Retain tombstones until all dispatch cursors have finished. Removing
      // the active handler must neither invalidate a cursor nor reuse its ID.
      collect();
      return 1;
    }
  return 0;
}
bool VectoredExceptions::recoverable(GuestArchitecture Architecture,
                                     const BackendFault &Fault) {
  if (Architecture == GuestArchitecture::X64 &&
      Fault.Kind == BackendFaultKind::Interrupt &&
      Fault.Interrupt == X64DivideVector)
    return true;
  return (Fault.Kind == BackendFaultKind::UnmappedMemory ||
          Fault.Kind == BackendFaultKind::Protection) &&
         Fault.Address && Fault.Size && Fault.Access &&
         (*Fault.Access == BackendAccessKind::Read ||
          *Fault.Access == BackendAccessKind::Write);
}
bool VectoredExceptions::accepts(const BackendFault &Fault) const {
  return recoverable(CPU.architecture(), Fault) &&
         llvm::any_of(Handlers, [](const auto &H) { return H.Live; });
}
VectoredExceptions::Exception
VectoredExceptions::exception(const BackendFault &Fault) {
  Exception E{uint32_t(Fault.Access ? StatusAccessViolation
                                    : StatusIntegerDivideByZero),
              0,
              Fault.PC,
              {}};
  if (Fault.Access)
    E.Arguments = {*Fault.Access == BackendAccessKind::Write ? ExceptionWrite
                                                             : ExceptionRead,
                   *Fault.Address};
  return E;
}
llvm::Expected<VectoredExceptions::Transfer>
VectoredExceptions::begin(Exception Raised, uint64_t StackPointer,
                          size_t LoaderDepth, std::optional<size_t> Event) {
  if (Frames.size() >= MaxExceptionDepth)
    return failure(text::ExceptionLimit);
  if (Raised.Arguments.size() > MaxExceptionArguments ||
      (Raised.Flags & ~(ExceptionNoncontinuable | ExceptionSoftwareOriginate)))
    return failure(text::ExceptionArguments);
  auto Context = captureUserContext(CPU);
  if (!Context)
    return Context.takeError();
  auto Snapshot = CPU.saveContext();
  if (!Snapshot)
    return Snapshot.takeError();
  const uint64_t Top = StackPointer & ~(ABI.info().StackAlignment - 1);
  if (Top <= StackBase || Top > StackTop)
    return failure(text::ExceptionFrame);
  auto Layout = ABI.layoutCall(StackBase, Top - StackBase, 1,
                               ExceptionContextOffset + Context->size());
  if (!Layout)
    return Layout.takeError();
  std::vector<uint8_t> Records(ExceptionContextOffset + Context->size());
  auto *P = Records.data();
  llvm::support::endian::write32le(P, Raised.Code);
  llvm::support::endian::write32le(P + ExceptionFlagsOffset, Raised.Flags);
  llvm::support::endian::write64le(P + ExceptionAddressOffset, Raised.Address);
  llvm::support::endian::write32le(P + ExceptionCountOffset,
                                   Raised.Arguments.size());
  for (size_t I = 0; I < Raised.Arguments.size(); ++I)
    llvm::support::endian::write64le(
        P + ExceptionArgumentsOffset + I * PointerSize, Raised.Arguments[I]);
  llvm::support::endian::write64le(P + ExceptionPointersOffset,
                                   Layout->PayloadAddress);
  llvm::support::endian::write64le(P + ExceptionPointersOffset + PointerSize,
                                   Layout->PayloadAddress +
                                       ExceptionContextOffset);
  std::copy(Context->begin(), Context->end(),
            Records.begin() + ExceptionContextOffset);
  auto Access = CPU.canAccess(Layout->StackPointer, Top - Layout->StackPointer,
                              Read | Write | UserAccessible);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return failure(text::ExceptionFrame);
  if (auto E = CPU.write(Layout->PayloadAddress, Records))
    return std::move(E);
  Frames.push_back({std::move(*Snapshot), std::move(*Context), Handlers.begin(),
                    Top, Layout->PayloadAddress, Layout->ReturnStackPointer,
                    LoaderDepth, Raised.Flags, Event});
  return callNext();
}
llvm::Expected<VectoredExceptions::Transfer> VectoredExceptions::callNext() {
  auto &F = Frames.back();
  while (F.Current != Handlers.end() && !F.Current->Live)
    ++F.Current;
  if (F.Current == Handlers.end())
    return failure(text::ExceptionUnhandled);
  const uint64_t PC = F.Current->PC;
  const bool X64 = CPU.architecture() == GuestArchitecture::X64;
  if (PC < ImageAlignment || PC >= UserLimit || (!X64 && PC % DWordSize))
    return failure(text::ExceptionHandler);
  auto Access =
      CPU.canAccess(PC, X64 ? 1 : DWordSize, Execute | UserAccessible);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return failure(text::ExceptionHandler);
  if (auto E = CPU.restoreContext(*F.Snapshot))
    return std::move(E);
  // The ABI reserves, but does not overwrite, the persistent exception records.
  auto Call =
      ABI.prepareCall(CPU, StackBase, F.Top - StackBase, ExceptionReturnGate,
                      {F.Payload + ExceptionPointersOffset},
                      ExceptionContextOffset + F.Context.size());
  if (!Call)
    return Call.takeError();
  if (auto E = CPU.writeRegister(
          X64 ? CPURegister::X64PC : CPURegister::AArch64PC, {PC, 0}))
    return std::move(E);
  return Transfer{PC, std::nullopt};
}
bool VectoredExceptions::activeAt(size_t LoaderDepth) const {
  return !Frames.empty() && Frames.back().LoaderDepth == LoaderDepth;
}
bool VectoredExceptions::returning(uint64_t PC, uint64_t SP,
                                   size_t LoaderDepth) const {
  return activeAt(LoaderDepth) && PC == ExceptionReturnGate &&
         SP == Frames.back().ExpectedSP;
}
llvm::Expected<VectoredExceptions::Transfer>
VectoredExceptions::returned(uint32_t Disposition) {
  auto &F = Frames.back();
  auto Pointers = CPU.canAccess(F.Payload + ExceptionPointersOffset,
                                2 * PointerSize, Read | UserAccessible);
  if (!Pointers)
    return Pointers.takeError();
  if (!*Pointers)
    return failure(text::ExceptionFrame);
  auto Record =
      CPU.readInteger(F.Payload + ExceptionPointersOffset, PointerSize);
  if (!Record)
    return Record.takeError();
  auto Context = CPU.readInteger(
      F.Payload + ExceptionPointersOffset + PointerSize, PointerSize);
  if (!Context)
    return Context.takeError();
  if (*Record != F.Payload || *Context != F.Payload + ExceptionContextOffset)
    return failure(text::ExceptionFrame);
  if (Disposition == ExceptionContinueSearch) {
    ++F.Current;
    return callNext();
  }
  if (Disposition != ExceptionContinueExecution)
    return failure(text::ExceptionDisposition);
  auto Access =
      CPU.canAccess(F.Payload, ExceptionContextOffset + F.Context.size(),
                    Read | UserAccessible);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return failure(text::ExceptionFrame);
  auto Flags = CPU.readInteger(F.Payload + ExceptionFlagsOffset, DWordSize);
  if (!Flags)
    return Flags.takeError();
  if ((F.Flags | *Flags) & ExceptionNoncontinuable)
    return failure(text::ExceptionContinuation);
  if (*Flags & ~ExceptionSoftwareOriginate)
    return failure(text::ExceptionContext);
  std::vector<uint8_t> Changed(F.Context.size());
  if (auto E = CPU.read(F.Payload + ExceptionContextOffset, Changed))
    return std::move(E);
  if (auto E = restoreUserContext(CPU, *F.Snapshot, F.Context, Changed,
                                  StackBase, StackTop))
    return std::move(E);
  auto PC = CPU.readRegister(CPU.architecture() == GuestArchitecture::X64
                                 ? CPURegister::X64PC
                                 : CPURegister::AArch64PC);
  if (!PC)
    return PC.takeError();
  Transfer T{(*PC)[0], F.Event};
  Frames.pop_back();
  collect();
  return T;
}
} // namespace neverd::emulation::windows_process
