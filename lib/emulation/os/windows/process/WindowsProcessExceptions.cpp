//===- WindowsProcessExceptions.cpp - Guest vectored exception dispatch --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessExceptions.h"

#include "../../../arch/x86_64/X64Exception.h"
#include "../exception/X64SIMDException.h"
#include "WindowsProcessContext.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::windows_process {
using namespace value;
static_assert(ExceptionArgumentsOffset + MaxExceptionArguments * PointerSize ==
              ExceptionRecordSize);
void ExceptionDispatcher::collect() {
  if (Frames.empty()) {
    Handlers.remove_if([](const auto &H) { return !H.Live; });
    ContinueHandlers.remove_if([](const auto &H) { return !H.Live; });
  }
}
std::list<ExceptionDispatcher::Handler> &
ExceptionDispatcher::handlers(HandlerKind Kind) {
  return Kind == HandlerKind::Exception ? Handlers : ContinueHandlers;
}
llvm::Expected<uint64_t> ExceptionDispatcher::add(HandlerKind Kind, bool First,
                                                  uint64_t PC) {
  collect();
  if (Handlers.size() + ContinueHandlers.size() >= MaxExceptionHandlers ||
      NextHandle >= UserLimit)
    return failure(text::ExceptionLimit);
  Handler H{NextHandle, PC};
  NextHandle += PointerSize;
  if (First)
    handlers(Kind).push_front(H);
  else
    handlers(Kind).push_back(H);
  return H.Handle;
}
uint64_t ExceptionDispatcher::remove(HandlerKind Kind, uint64_t Handle) {
  for (auto &H : handlers(Kind))
    if (H.Handle == Handle && H.Live) {
      H.Live = false;
      // Retain tombstones until all dispatch cursors have finished. Removing
      // the active handler must neither invalidate a cursor nor reuse its ID.
      collect();
      return 1;
    }
  return 0;
}
bool ExceptionDispatcher::recoverable(GuestArchitecture Architecture,
                                      const BackendFault &Fault,
                                      std::optional<uint64_t> MXCSR) {
  return exception(Architecture, Fault, MXCSR).has_value();
}
bool ExceptionDispatcher::accepts(const BackendFault &Fault) const {
  auto Raised = exception(Fault);
  if (!Raised) {
    // A failed state read cannot authorize recovery. Leave the CPU's original
    // fault terminal instead of fabricating a Windows exception record.
    llvm::consumeError(Raised.takeError());
    return false;
  }
  return Raised->has_value() &&
         (llvm::any_of(Handlers, [](const auto &H) { return H.Live; }) ||
          hasFrameHandlers());
}
llvm::Expected<std::optional<ExceptionDispatcher::Exception>>
ExceptionDispatcher::exception(const BackendFault &Fault) const {
  std::optional<uint64_t> MXCSR;
  if (CPU.architecture() == GuestArchitecture::X64 &&
      Fault.Kind == BackendFaultKind::Interrupt &&
      Fault.Interrupt == unsigned(x64::ExceptionVector::SIMD)) {
    auto Control = CPU.reg(X64Register::MXCSR);
    if (!Control)
      return Control.takeError();
    MXCSR = *Control;
  }
  return exception(CPU.architecture(), Fault, MXCSR);
}
std::optional<ExceptionDispatcher::Exception>
ExceptionDispatcher::exception(GuestArchitecture Architecture,
                               const BackendFault &Fault,
                               std::optional<uint64_t> MXCSR) {
  if (Fault.Cause) {
    // The shared architecture owns the cause. Other #GP(0) results remain
    // unclassified: their Windows status cannot be inferred from the vector.
    if (Architecture != GuestArchitecture::X64 ||
        *Fault.Cause != BackendFaultCause::OperandAlignment ||
        Fault.Kind != BackendFaultKind::Interrupt ||
        Fault.Interrupt != unsigned(x64::ExceptionVector::GeneralProtection) ||
        Fault.ErrorCode != x64::NoSelectorErrorCode || Fault.Address ||
        Fault.Size || Fault.Access)
      return std::nullopt;
    return Exception{StatusAccessViolation,
                     0,
                     Fault.PC,
                     {ExceptionRead, ExceptionUnknownAddress}};
  }
  if (Architecture == GuestArchitecture::X64 &&
      Fault.Kind == BackendFaultKind::Interrupt &&
      Fault.Interrupt == X64DivideVector)
    return Exception{StatusIntegerDivideByZero, 0, Fault.PC, {}};
  if (Architecture == GuestArchitecture::X64 &&
      Fault.Kind == BackendFaultKind::Interrupt &&
      Fault.Interrupt == unsigned(x64::ExceptionVector::SIMD)) {
    if (!MXCSR || Fault.ErrorCode || Fault.Address || Fault.Size ||
        Fault.Access)
      return std::nullopt;
    auto SIMD = windows_exception::x64SIMDException(*MXCSR);
    if (!SIMD)
      return std::nullopt;
    return Exception{SIMD->Code,
                     0,
                     Fault.PC,
                     {SIMD->Parameters.begin(), SIMD->Parameters.end()}};
  }
  if ((Fault.Kind == BackendFaultKind::UnmappedMemory ||
       Fault.Kind == BackendFaultKind::Protection) &&
      Fault.Address && Fault.Size && Fault.Access &&
      (*Fault.Access == BackendAccessKind::Read ||
       *Fault.Access == BackendAccessKind::Write))
    return Exception{StatusAccessViolation,
                     0,
                     Fault.PC,
                     {*Fault.Access == BackendAccessKind::Write ? ExceptionWrite
                                                                : ExceptionRead,
                      *Fault.Address}};
  return std::nullopt;
}
llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::begin(Exception Raised, uint64_t StackPointer,
                           size_t LoaderDepth, std::optional<size_t> Event) {
  return beginDispatch(std::move(Raised), StackPointer, LoaderDepth, Event, {});
}
llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::beginFault(const BackendFault &Fault,
                                uint64_t StackPointer, size_t LoaderDepth) {
  auto Raised = exception(Fault);
  if (!Raised)
    return Raised.takeError();
  if (!*Raised)
    return failure(text::ExceptionContext);
  return beginDispatch(std::move(**Raised), StackPointer, LoaderDepth, {}, {},
                       ContextOrigin::HardwareFault);
}
llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::beginDispatch(Exception Raised, uint64_t StackPointer,
                                   size_t LoaderDepth,
                                   std::optional<size_t> Event,
                                   std::optional<size_t> Rejected,
                                   ContextOrigin Origin) {
  if (Frames.size() >= MaxExceptionDepth)
    return failure(text::ExceptionLimit);
  if (Raised.Arguments.size() > MaxExceptionArguments ||
      (Raised.Flags & ~(ExceptionNoncontinuable | ExceptionSoftwareOriginate)))
    return failure(text::ExceptionArguments);
  auto Context = captureUserContext(CPU, Origin);
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
                    LoaderDepth, Event});
  Frames.back().Rejected = Rejected;
  Frames.back().Origin = Origin;
  return callNext();
}
llvm::Expected<ExceptionDispatcher::Transfer> ExceptionDispatcher::callNext() {
  auto &F = Frames.back();
  auto &List = handlers(F.Kind);
  while (F.Current != List.end() && !F.Current->Live)
    ++F.Current;
  if (F.Current == List.end()) {
    if (F.Kind == HandlerKind::Exception)
      return startUnwind();
    return continueExecution();
  }
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
bool ExceptionDispatcher::activeAt(size_t LoaderDepth) const {
  return !Frames.empty() && Frames.back().LoaderDepth == LoaderDepth;
}
bool ExceptionDispatcher::returning(uint64_t PC, uint64_t SP,
                                    size_t LoaderDepth) const {
  return activeAt(LoaderDepth) && PC == ExceptionReturnGate &&
         SP == Frames.back().ExpectedSP;
}
llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::returned(uint32_t Disposition) {
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
  if (F.SEH && F.SEH->Callback) {
    if (auto E = validateUnwind())
      return std::move(E);
    auto Accessible =
        CPU.canAccess(F.Payload, ExceptionRecordSize, Read | UserAccessible);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return failure(text::ExceptionFrame);
    std::vector<uint8_t> RecordBytes(ExceptionRecordSize);
    if (auto E = CPU.read(F.Payload, RecordBytes))
      return std::move(E);
    if (RecordBytes != F.SEH->Record)
      return failure(text::ExceptionContext);
    const auto Kind = F.SEH->Callback->Kind;
    F.SEH->Callback.reset();
    return advanceUnwind(Kind == X64SEH::ActionKind::Filter
                             ? std::optional<int32_t>(Disposition)
                             : std::nullopt);
  }
  if (Disposition == ExceptionContinueSearch) {
    ++F.Current;
    return callNext();
  }
  if (Disposition != ExceptionContinueExecution)
    return failure(text::ExceptionDisposition);
  if (F.Kind == HandlerKind::Exception) {
    // Continue callbacks observe the same live records, including edits made
    // by exception handlers. Validate the final context only after they finish.
    F.Kind = HandlerKind::Continue;
    F.Current = ContinueHandlers.begin();
    return callNext();
  }
  return continueExecution();
}
llvm::Expected<ExceptionDispatcher::Transfer>
ExceptionDispatcher::continueExecution() {
  auto &F = Frames.back();
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
  // Native vectored continuation does not apply the frame-based SEH
  // noncontinuable check. Preserve the flag without inventing a second raise.
  if (*Flags & ~(ExceptionNoncontinuable | ExceptionSoftwareOriginate))
    return failure(text::ExceptionContext);
  // Native dispatcher-generated noncontinuable exceptions remain terminal
  // after VEH/VCH accept continuation, including edits to the saved CONTEXT.
  // Keep that origin independently of guest-mutable record flags and code.
  if (F.Rejected)
    return failure(text::ExceptionUnhandled);
  std::vector<uint8_t> Changed(F.Context.size());
  if (auto E = CPU.read(F.Payload + ExceptionContextOffset, Changed))
    return std::move(E);
  if (auto E = restoreUserContext(CPU, *F.Snapshot, F.Context, Changed,
                                  StackBase, StackTop, F.Origin))
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
