//===- DarwinCallbacks.cpp - Startup and TLS termination frames -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file One process lifecycle; guest callbacks share the execution session.
#include "DarwinCallbacks.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::emulation::darwin_model {
namespace {
llvm::Expected<IntegerABI> abi(const ProcessImage &Image) {
  return IntegerABI::get(Image.Architecture == GuestArchitecture::X64
                             ? IntegerCallingConvention::SysVAMD64
                             : IntegerCallingConvention::AAPCS64);
}
CPURegister pc(const ProcessImage &Image) {
  return Image.Architecture == GuestArchitecture::X64 ? CPURegister::X64PC
                                                      : CPURegister::AArch64PC;
}
} // namespace
DarwinCallbacks::DarwinCallbacks(const ProcessImage &Image,
                                 const ProcessOptions &Options,
                                 const InitialStack &Stack)
    : Image(Image), Stack(Stack),
      StackBase(value::StackTop - Options.StackSize) {
  if (Image.HasTLSCallbacks)
    Pending.reserve(TLSCallbackLimit);
}
llvm::Error DarwinCallbacks::target(ExecutionBackend &CPU,
                                    uint64_t Address) const {
  const uint64_t Width = Image.Architecture == GuestArchitecture::X64 ? 1 : 4;
  if (!Address || (Width == 4 && Address % 4) ||
      !llvm::any_of(Image.ExecutableSpans, [&](const auto &S) {
        return Address >= S.Address && Width <= S.Size &&
               Address - S.Address <= S.Size - Width;
      }))
    return failure(
        "Darwin callback target is outside original executable storage");
  auto Access = CPU.canAccess(Address, Width, Execute | UserAccessible);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return failure("Darwin callback target is no longer executable");
  return llvm::Error::success();
}
llvm::Error DarwinCallbacks::enter(ExecutionBackend &CPU, uint64_t Address,
                                   uint64_t Top,
                                   llvm::ArrayRef<uint64_t> Arguments,
                                   bool Callback) {
  if (Callback)
    if (auto E = target(CPU, Address))
      return E;
  if (Top <= StackBase || Top > Stack.SP)
    return failure("Darwin callback stack escapes process startup storage");
  auto ABI = abi(Image);
  if (!ABI)
    return ABI.takeError();
  auto Call =
      ABI->prepareCall(CPU, StackBase, Top - StackBase,
                       Callback ? ReturnGate : value::ReturnGate, Arguments);
  if (!Call)
    return Call.takeError();
  if (auto E = CPU.writeRegister(pc(Image), {Address, 0}))
    return E;
  if (Callback)
    Active = Frame{Address, Call->ReturnStackPointer};
  else
    MainReturnSP = Call->ReturnStackPointer;
  return llvm::Error::success();
}
llvm::Error DarwinCallbacks::startup(ExecutionBackend &CPU) {
  if (NextInitializer < Image.Initializers.size()) {
    const auto &Init = Image.Initializers[NextInitializer];
    auto Word = CPU.readInteger(Init.Slot, Init.Width);
    if (!Word)
      return Word.takeError();
    if (*Word != Init.Word)
      return failure("Darwin pending initializer table was modified");
    const uint64_t Args[] = {Stack.Argc, Stack.Argv, Stack.Envp, Stack.Apple,
                             Stack.ProgramVars};
    return enter(CPU, Init.Target, Stack.SP, Args, true);
  }
  State = Phase::Main;
  const uint64_t Args[] = {Stack.Argc, Stack.Argv, Stack.Envp, Stack.Apple};
  return enter(CPU, Image.Plan.Entry, Stack.SP, Args, false);
}
llvm::Expected<uint64_t> DarwinCallbacks::start(ExecutionBackend &CPU) {
  if (Image.MainEntry) {
    if (auto E = startup(CPU))
      return std::move(E);
    return Active ? Active->Target : Image.Plan.Entry;
  }
  State = Phase::Main;
  auto ABI = abi(Image);
  if (!ABI)
    return ABI.takeError();
  if (auto E = CPU.writeRegister(ABI->info().StackPointer, {Stack.SP, 0}))
    return std::move(E);
  if (auto E = CPU.writeRegister(pc(Image), {Image.Plan.Entry, 0}))
    return std::move(E);
  return Image.Plan.Entry;
}
llvm::Error DarwinCallbacks::addTLS(ExecutionBackend &CPU, uint64_t Function,
                                    uint64_t Object) {
  if (!Image.HasTLSCallbacks || State == Phase::Complete)
    return failure("Darwin TLS callback registration is unavailable");
  if (Pending.size() == TLSCallbackLimit)
    return failure("Darwin TLS callback registration limit exceeded");
  if (Function)
    if (auto E = target(CPU, Function))
      return E;
  Pending.push_back({Function, Object});
  return llvm::Error::success();
}
llvm::Expected<bool> DarwinCallbacks::terminate(ExecutionBackend &CPU,
                                                ProcessResult &Result) {
  while (!Pending.empty()) {
    const auto Callback = Pending.back();
    Pending.pop_back(); // A callback can register another before older entries.
    if (!Callback.Function)
      continue;
    const uint64_t Args[] = {Callback.Object};
    if (auto E = enter(CPU, Callback.Function, TerminationTop, Args, true))
      return std::move(E);
    Result.PC = Callback.Function;
    return true;
  }
  State = Phase::Complete;
  Result.Stop = ProcessStopReason::Exited;
  Result.ExitStatus = ExitStatus;
  Result.ReturnValue = MainValue;
  return false;
}
llvm::Expected<bool>
DarwinCallbacks::exit(ExecutionBackend &CPU, uint64_t Status,
                      ProcessResult &Result,
                      std::optional<uint64_t> ReturnValue) {
  if (State == Phase::Termination || State == Phase::Complete)
    return failure(
        "reentrant Darwin exit during TLS termination is unsupported");
  auto ABI = abi(Image);
  if (!ABI)
    return ABI.takeError();
  auto SP = CPU.readRegister(ABI->info().StackPointer);
  if (!SP)
    return SP.takeError();
  if ((*SP)[0] > Stack.SP || (*SP)[0] < StackBase ||
      (*SP)[0] - StackBase < ABI->info().RedZoneSize)
    return failure("invalid Darwin termination stack");
  // Preserve live caller locals and the current x64 red zone on deep exit().
  TerminationTop = ((*SP)[0] - ABI->info().RedZoneSize) & ~uint64_t(15);
  ExitStatus = Status & 0xff;
  MainValue = ReturnValue;
  Active.reset();
  State = Phase::Termination;
  return terminate(CPU, Result);
}
llvm::Expected<bool> DarwinCallbacks::returned(ExecutionBackend &CPU,
                                               uint64_t Gate,
                                               ProcessResult &Result) {
  auto ABI = abi(Image);
  if (!ABI)
    return ABI.takeError();
  auto SP = CPU.readRegister(ABI->info().StackPointer);
  if (!SP)
    return SP.takeError();
  if (Gate == value::ReturnGate) {
    if (!Image.MainEntry || State != Phase::Main || Active ||
        (*SP)[0] != MainReturnSP)
      return failure("invalid Darwin main return frame");
    auto Value = CPU.readRegister(ABI->info().Result);
    if (!Value)
      return Value.takeError();
    return exit(CPU, (*Value)[0], Result, (*Value)[0]);
  }
  if (Gate != ReturnGate || !Active || (*SP)[0] != Active->ReturnSP ||
      (State != Phase::Startup && State != Phase::Termination))
    return failure("invalid Darwin callback return frame");
  if (auto E = target(CPU, Active->Target))
    return std::move(E);
  Active.reset();
  if (State == Phase::Termination)
    return terminate(CPU, Result);
  ++NextInitializer;
  if (auto E = startup(CPU))
    return std::move(E);
  Result.PC = Active ? Active->Target : Image.Plan.Entry;
  return true;
}
} // namespace neverd::emulation::darwin_model
