//===- GuestInvocation.cpp - ABI state without a second execution pump ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ABI state without a second execution pump.
///

#include "neverd/emulation/GuestInvocation.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/ExecutionLimits.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
bool contains(uint64_t Base, uint64_t Size, uint64_t Address) {
  return Address >= Base && Address - Base < Size;
}
uint64_t alignedSize(uint64_t Size, uint64_t Alignment) {
  return (Size + Alignment - 1) & ~(Alignment - 1);
}
} // namespace

llvm::Error GuestInvocation::validate(const GuestInvocationRequest &R,
                                      GuestArchitecture Architecture,
                                      uint64_t ByteLimit) {
  if (!R.ABI || R.InstanceID.size() > execution_limit::NameBytes ||
      R.InstanceID.find('\0') != std::string::npos ||
      R.Arguments.size() > execution_limit::Arguments ||
      R.Buffers.size() > execution_limit::Buffers || !R.Limits.Instructions ||
      !R.Limits.Events || !R.Limits.TimeoutMicroseconds || !ByteLimit ||
      ByteLimit > execution_limit::TransferBytes)
    return failure("invalid finite guest invocation bounds or missing ABI");
  auto ABI = IntegerABI::get(*R.ABI);
  if (!ABI)
    return ABI.takeError();
  if (ABI->info().Architecture != Architecture)
    return failure("guest invocation ABI does not match the CPU architecture");
  uint64_t Total = 0;
  for (const auto &B : R.Buffers) {
    if (B.Size > ByteLimit || B.Input.size() > B.Size)
      return failure("guest invocation buffer exceeds its bounds");
    const auto Size = alignedSize(B.Size, ABI->info().StackAlignment);
    if (Size > ByteLimit - Total)
      return failure("guest invocation payload exceeds the transfer bound");
    Total += Size;
  }
  for (const auto &A : R.Arguments)
    if (A.Buffer &&
        (*A.Buffer >= R.Buffers.size() || A.Value > R.Buffers[*A.Buffer].Size))
      return failure("guest invocation argument refers outside its buffer");
  return llvm::Error::success();
}

GuestInvocation::GuestInvocation(GuestInvocationRequest Request, IntegerABI ABI,
                                 GuestInvocationFrame Frame,
                                 uint64_t PayloadSize,
                                 std::vector<uint64_t> Addresses,
                                 AccountingStart Start)
    : Request(std::move(Request)), ABI(ABI), Frame(Frame),
      PayloadSize(PayloadSize), StartInstructions(Start.Instructions),
      StartEvents(Start.Events), StartMicroseconds(Start.RemainingMicroseconds),
      Addresses(std::move(Addresses)) {}

llvm::Expected<std::unique_ptr<GuestInvocation>> GuestInvocation::prepare(
    GuestInvocationRequest Request, GuestInvocationPlan Plan, uint64_t Entry,
    ExecutionSession &Session, uint64_t ByteLimit, AccountingStart Start) {
  if (!Session.readyToRun() || Session.hasResourceContinuation())
    return failure("guest invocation requires an available stopped session");
  if (auto E = validate(Request, Session.cpu().architecture(), ByteLimit))
    return std::move(E);
  auto ABI = IntegerABI::get(*Request.ABI);
  if (!ABI)
    return ABI.takeError();
  if (!Entry || !Plan.ReturnPC || Entry == Plan.ReturnPC || !Plan.StackSize ||
      Plan.StackSize > execution_limit::TransferBytes ||
      Plan.StackSize > UINT64_MAX - Plan.StackBase ||
      contains(Plan.StackBase, Plan.StackSize, Entry) ||
      contains(Plan.StackBase, Plan.StackSize, Plan.ReturnPC) ||
      (ABI->info().Architecture == GuestArchitecture::AArch64 &&
       ((Entry | Plan.ReturnPC) & 3)))
    return failure("invalid guest invocation stack or return boundary");
  auto SP = Session.cpu().readRegister(ABI->info().StackPointer);
  if (!SP)
    return SP.takeError();
  if (contains(Plan.StackBase, Plan.StackSize, (*SP)[0]))
    return failure("guest invocation stack overlaps the suspended caller");
  const auto Space = Session.cpu().addressSpace();
  auto StackAccess =
      Space->canAccess(Plan.StackBase, Plan.StackSize, Read | Write);
  if (!StackAccess)
    return StackAccess.takeError();
  auto ReturnAccess = Space->canAccess(Plan.ReturnPC, 1, Execute);
  if (!ReturnAccess)
    return ReturnAccess.takeError();
  if (!*StackAccess || !*ReturnAccess)
    return failure(
        "guest invocation requires RAM stack and executable return boundary");
  uint64_t Payload = 0;
  std::vector<uint64_t> Offsets;
  for (const auto &B : Request.Buffers) {
    Offsets.push_back(Payload);
    Payload += alignedSize(B.Size, ABI->info().StackAlignment);
  }
  auto Layout = ABI->layoutCall(Plan.StackBase, Plan.StackSize,
                                Request.Arguments.size(), Payload);
  if (!Layout)
    return Layout.takeError();
  for (auto &Offset : Offsets)
    Offset += Layout->PayloadAddress;
  GuestInvocationFrame Frame{
      Entry,          Plan.ReturnPC,        Plan.StackBase,
      Plan.StackSize, Layout->StackPointer, Layout->ReturnStackPointer};
  return std::unique_ptr<GuestInvocation>(new GuestInvocation(
      std::move(Request), *ABI, Frame, Payload, std::move(Offsets), Start));
}

llvm::Error GuestInvocation::install(ExecutionSession &Session) {
  if (Caller)
    return failure("guest invocation frame is already installed");
  auto Saved = Session.saveContext();
  if (!Saved)
    return Saved.takeError();
  Caller = std::move(*Saved);
  if (auto E = Session.watchReturn(Frame.ReturnPC))
    return E;
  // Environment instruction watches belong to the suspended caller. The
  // reserved return remains active even when the environment changes watches.
  if (auto E = Session.watchExecution({}))
    return E;
  if (auto E = Session.watchMemoryWrites({}))
    return E;
  std::vector<uint64_t> Arguments;
  for (const auto &A : Request.Arguments)
    Arguments.push_back(A.Buffer ? Addresses[*A.Buffer] + A.Value : A.Value);
  auto Layout = ABI.prepareCall(Session.cpu(), Frame.StackBase, Frame.StackSize,
                                Frame.ReturnPC, Arguments, PayloadSize);
  if (!Layout)
    return Layout.takeError();
  std::vector<uint8_t> Payload(PayloadSize, 0);
  const uint64_t Base = Frame.StackBase + Frame.StackSize - PayloadSize;
  for (size_t I = 0; I < Request.Buffers.size(); ++I)
    std::copy(Request.Buffers[I].Input.begin(), Request.Buffers[I].Input.end(),
              Payload.begin() + (Addresses[I] - Base));
  return Session.cpu().addressSpace()->write(Base, Payload);
}

GuestInvocationProgress
GuestInvocation::progress(const ExecutionBudget &Budget) const {
  const auto Instructions = Budget.instructions() - StartInstructions;
  const auto Events = Budget.events() - StartEvents;
  const auto Now = Budget.remainingMicroseconds();
  const auto Elapsed = StartMicroseconds - std::min(StartMicroseconds, Now);
  return {
      Frame,
      Instructions,
      Events,
      Elapsed,
      Request.Limits.Instructions -
          std::min(Request.Limits.Instructions, Instructions),
      Request.Limits.Events - std::min(Request.Limits.Events, Events),
      std::min(Now, Request.Limits.TimeoutMicroseconds -
                        std::min(Request.Limits.TimeoutMicroseconds, Elapsed))};
}

llvm::Expected<GuestInvocationResult>
GuestInvocation::result(ExecutionSession &Session) const {
  if (!Caller || !Session.hasInvocationReturn() || Session.cpu().fault())
    return failure("guest invocation has no successful return boundary");
  auto SP = Session.cpu().readRegister(ABI.info().StackPointer);
  if (!SP)
    return SP.takeError();
  if ((*SP)[0] != Frame.ReturnStackPointer)
    return failure("guest invocation returned with an unbalanced stack");
  auto Value = Session.cpu().readRegister(ABI.info().Result);
  if (!Value)
    return Value.takeError();
  const auto Progress = progress(Session.budget());
  GuestInvocationResult Result{
      Frame.Entry,
      (*Value)[0],
      Progress.Instructions,
      Progress.Events,
      StartMicroseconds -
          std::min(StartMicroseconds, Session.budget().remainingMicroseconds()),
      {}};
  for (size_t I = 0; I < Request.Buffers.size(); ++I) {
    const auto &B = Request.Buffers[I];
    GuestInvocationOutput Output{Addresses[I],
                                 std::vector<uint8_t>(B.Capture ? B.Size : 0)};
    if (auto E =
            Session.cpu().addressSpace()->read(Output.Address, Output.Bytes))
      return std::move(E);
    Result.Buffers.push_back(std::move(Output));
  }
  return Result;
}

llvm::Error GuestInvocation::restoreCaller(ExecutionSession &Session) {
  if (!Caller || !Session.hasInvocationReturn() || Session.cpu().fault())
    return failure("guest invocation has no validated return to its caller");
  if (auto E = Session.restoreContext(*Caller))
    return E;
  Caller.reset();
  return llvm::Error::success();
}
} // namespace neverd::emulation
