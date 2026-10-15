//===- DriverObservation.cpp - Stopped driver inspection ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DriverObservation.h"

#include "../../../arch/x86_64/X64Machine.h"
#include "../kernel/KernelModel.h"
#include "DriverImage.h"

#include "neverd/emulation/IntegerABI.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <utility>

namespace neverd::emulation {
namespace {
// DriverEntry has two incoming arguments. A new loader call must also retain
// its caller's nonvolatile registers and return/shadow frame.
constexpr CPURegister PreservedRegisters[] = {
    CPURegister::X64CX,  CPURegister::X64DX,  CPURegister::X64BX,
    CPURegister::X64BP,  CPURegister::X64SI,  CPURegister::X64DI,
    CPURegister::X64R12, CPURegister::X64R13, CPURegister::X64R14,
    CPURegister::X64R15, CPURegister::X64V6,  CPURegister::X64V7,
    CPURegister::X64V8,  CPURegister::X64V9,  CPURegister::X64V10,
    CPURegister::X64V11, CPURegister::X64V12, CPURegister::X64V13,
    CPURegister::X64V14, CPURegister::X64V15, CPURegister::X64MXCSR,
    CPURegister::X64FPCW};

bool entryContextUnchanged(const WindowsDriverState::EntryContext &Entry) {
  if (!Entry.DriverEntryActive || Entry.Frame != Entry.InitialFrame)
    return false;
  const auto &Before = Entry.InitialRegisters;
  const auto &After = Entry.Registers;
  if (Before.at(CPURegister::X64SP) != After.at(CPURegister::X64SP) ||
      ((Before.at(CPURegister::X64FLAGS)[0] ^
        After.at(CPURegister::X64FLAGS)[0]) &
       x64::DirectionFlag))
    return false;
  return std::all_of(std::begin(PreservedRegisters),
                     std::end(PreservedRegisters),
                     [&](auto R) { return Before.at(R) == After.at(R); });
}

} // namespace

DriverObservation::DriverObservation(
    ProcessObserver *Observer, ExecutionBackend &CPU, const DriverImage &Image,
    KernelModel &Kernel, const KernelExportRegistry &Exports,
    const DriverResult &Result, std::vector<RelocationWitness> RelocationWrites,
    std::string Name)
    : Observer(Observer), CPU(CPU), Image(Image), Kernel(Kernel),
      Exports(Exports), Result(Result),
      RelocationWrites(std::move(RelocationWrites)), Name(std::move(Name)) {}

llvm::Expected<std::vector<DriverObservation::RelocationWitness>>
DriverObservation::watchRelocations(ExecutionBackend &CPU,
                                    const DriverImage &Image) {
  std::vector<RelocationWitness> Out;
  for (const auto &Field : Image.Relocations) {
    auto Bytes = CPU.addressSpace()->pinBacking(Image.Base + Field.RVA, 8);
    if (!Bytes)
      return Bytes.takeError();
    auto Writes = Bytes->watchWrites();
    if (!Writes)
      return Writes.takeError();
    std::array<uint8_t, 8> Value;
    if (auto E = Bytes->read(0, Value))
      return std::move(E);
    if (llvm::support::endian::read64le(Value.data()) != Field.LoadedValue)
      return llvm::createStringError(
          "mapped driver relocation differs from the validated loader field");
    Out.push_back({std::move(*Bytes), std::move(*Writes)});
  }
  return Out;
}

llvm::Error
DriverObservation::replaceWatches(std::vector<ExecutionWatch> Next) {
  for (const auto &W : Next)
    if (!W.Size || W.Size - 1 > UINT64_MAX - W.Address)
      return llvm::createStringError("invalid driver execution watch");
  ExecutionWatchesDirty |=
      Next.size() != Watches.size() ||
      !std::equal(Next.begin(), Next.end(), Watches.begin(),
                  [](const auto &A, const auto &B) {
                    return A.Address == B.Address && A.Size == B.Size;
                  });
  Watches = std::move(Next);
  return llvm::Error::success();
}

llvm::Error DriverObservation::entered(bool Entry, ProcessStackView NewStack) {
  MemoryUnchanged = false;
  EntryInvocation = Entry;
  Stack = NewStack;
  if (!Observer)
    return llvm::Error::success();
  if (!Started) {
    if (!Entry)
      return llvm::createStringError(
          "driver observation must start at DriverEntry");
    if (auto E = Kernel.captureUnpackBaseline())
      return E;
    auto Registers = entryRegisters();
    if (!Registers)
      return Registers.takeError();
    InitialRegisters = std::move(*Registers);
    if (auto E = read(InitialRegisters.at(CPURegister::X64SP)[0], InitialFrame))
      return E;
    // The initial stopped observer may query the completed invocation baseline.
    Started = true;
    auto Next = Observer->started(*this);
    if (!Next)
      return Next.takeError();
    return replaceWatches(std::move(*Next));
  }
  auto Next = Observer->invoking(*this);
  if (!Next)
    return Next.takeError();
  if (*Next)
    return replaceWatches(std::move(**Next));
  return llvm::Error::success();
}

llvm::Error DriverObservation::resuming() {
  if (Observer) {
    auto Next = Observer->resuming(*this);
    if (!Next)
      return Next.takeError();
    if (*Next)
      if (auto E = replaceWatches(std::move(**Next)))
        return E;
    if (auto E = CPU.setMemoryWriteWatches(Observer->writeWatches()))
      return E;
  }
  return installExecutionWatches();
}

void DriverObservation::setExecutionGates(std::vector<ExecutionWatch> Gates) {
  ExecutionGates = std::move(Gates);
  ExecutionWatchesDirty = true;
}

llvm::Error DriverObservation::installExecutionWatches() {
  if (ExecutionGates.empty() || !ExecutionWatchesDirty)
    return llvm::Error::success();
  auto Combined = ExecutionGates;
  Combined.insert(Combined.end(), Watches.begin(), Watches.end());
  std::sort(Combined.begin(), Combined.end(),
            [](const auto &A, const auto &B) { return A.Address < B.Address; });
  size_t Count = 0;
  for (const auto W : Combined) {
    if (Count &&
        W.Address - Combined[Count - 1].Address <= Combined[Count - 1].Size) {
      auto &Previous = Combined[Count - 1];
      const uint64_t Last = std::max(Previous.Address + Previous.Size - 1,
                                     W.Address + W.Size - 1);
      Previous.Size = Last - Previous.Address + 1;
      if (!Previous.Size)
        return llvm::createStringError("invalid driver execution watch union");
    } else
      Combined[Count++] = W;
  }
  Combined.resize(Count);
  CPU.setExecutionWatches(Combined);
  ExecutionWatchesDirty = false;
  return llvm::Error::success();
}

bool DriverObservation::matches(uint64_t PC, bool Direct) {
  const bool Resumed = ResumePC == PC;
  if (Direct)
    ResumePC.reset();
  if (Resumed)
    return false;
  ResumePC.reset();
  return std::any_of(Watches.begin(), Watches.end(), [&](const auto &W) {
    return PC >= W.Address && PC - W.Address < W.Size;
  });
}

void DriverObservation::instructionAdmitted() {
  ResumePC.reset();
  ExecutedPrefix = true;
}

void DriverObservation::opaqueExecutionResuming() {
  // A direct run can modify incoming state even when the stopped bytes and
  // registers compare equal. It can also observe processor identity/time
  // without an instruction callback. Neither fact follows from a zero count.
  ExecutedPrefix = true;
  Kernel.recordUnpackEnvironmentRead();
}

llvm::Expected<bool> DriverObservation::watched(uint64_t PC) {
  auto Next = Observer->watched(*this, PC);
  if (!Next)
    return Next.takeError();
  if (!*Next)
    return false;
  if (auto E = replaceWatches(std::move(**Next)))
    return std::move(E);
  ResumePC = PC;
  return true;
}

llvm::Error
DriverObservation::exporting(const KernelExportRegistry::Export &Export) {
  if (!Observer ||
      Export.Kind != KernelExportRegistry::ExportKind::ModuleExport)
    return llvm::Error::success();
  std::optional<uint64_t> Return;
  auto SP = CPU.reg(X64Register::SP);
  if (!SP)
    return SP.takeError();
  std::array<uint8_t, 8> Bytes;
  if (auto E = read(*SP, Bytes))
    llvm::consumeError(std::move(E));
  else
    Return = llvm::support::endian::read64le(Bytes.data());
  return Observer->exporting(
      *this, {Export.Address, Export.Module, Export.Name, std::nullopt},
      Return);
}

GuestArchitecture DriverObservation::architecture() const {
  return CPU.architecture();
}
llvm::Expected<RegisterValue>
DriverObservation::readRegister(CPURegister Register) {
  return CPU.readRegister(Register);
}
llvm::Error DriverObservation::read(uint64_t Address,
                                    llvm::MutableArrayRef<uint8_t> Bytes) {
  return CPU.addressSpace()->snapshotBacking(Address, Bytes);
}
llvm::Expected<std::vector<AddressMapping>> DriverObservation::mappings() {
  return CPU.addressSpace()->mappings();
}
std::vector<ProcessModuleView> DriverObservation::modules() {
  return {{Name, Image.Base, Image.Size, Image.Entry, true, false}};
}
std::vector<ProcessExportView> DriverObservation::exports() {
  std::vector<ProcessExportView> Out;
  for (const auto &[Address, E] : Exports.entries())
    if (E.Kind == KernelExportRegistry::ExportKind::ModuleExport)
      Out.push_back({Address, E.Module, E.Name, std::nullopt});
  return Out;
}
llvm::Expected<std::optional<ProcessCallFrame>> DriverObservation::callFrame() {
  auto SP = CPU.reg(X64Register::SP);
  if (!SP)
    return SP.takeError();
  auto ABI = llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));
  if (auto E = ABI.validateStackPointer(*SP)) {
    llvm::consumeError(std::move(E));
    return std::nullopt;
  }
  std::array<uint8_t, 8> Bytes;
  if (auto E = read(*SP, Bytes)) {
    llvm::consumeError(std::move(E));
    return std::nullopt;
  }
  const uint64_t Return = llvm::support::endian::read64le(Bytes.data());
  if (!CPU.executable(Return))
    return std::nullopt;
  auto ReturnSP = ABI.returnStackPointer(*SP);
  if (!ReturnSP)
    return ReturnSP.takeError();
  ProcessCallFrame Frame{Return, *ReturnSP, {}};
  for (size_t I = 0; I < Frame.Arguments.size(); ++I) {
    auto V = ABI.readArgument(CPU, *SP, I);
    if (!V)
      return V.takeError();
    Frame.Arguments[I] = *V;
  }
  return Frame;
}
std::optional<std::vector<ProcessHeapAllocationView>>
DriverObservation::heapAllocations() const {
  std::vector<ProcessHeapAllocationView> Out;
  for (const auto &[Address, Size] : Kernel.unpackAllocations())
    Out.push_back({Address, Size});
  return Out;
}
llvm::Expected<WindowsDriverState::RegisterFile>
DriverObservation::entryRegisters() {
  WindowsDriverState::RegisterFile Registers;
  auto Capture = [&](CPURegister R) -> llvm::Error {
    auto V = readRegister(R);
    if (!V)
      return V.takeError();
    Registers.emplace(R, *V);
    return llvm::Error::success();
  };
  for (auto R : PreservedRegisters)
    if (auto E = Capture(R))
      return std::move(E);
  for (auto R : {CPURegister::X64AX, CPURegister::X64SP, CPURegister::X64PC,
                 CPURegister::X64FLAGS, CPURegister::X64R8, CPURegister::X64R9,
                 CPURegister::X64R10, CPURegister::X64R11, CPURegister::X64V0,
                 CPURegister::X64V1, CPURegister::X64V2, CPURegister::X64V3,
                 CPURegister::X64V4, CPURegister::X64V5})
    if (auto E = Capture(R))
      return std::move(E);
  return Registers;
}

llvm::Expected<WindowsDriverState::EntryContext>
DriverObservation::entryContext(bool IncludeBacking) {
  if (!Started)
    return llvm::createStringError(
        "driver recovery has no invocation baseline");
  auto Registers = entryRegisters();
  if (!Registers)
    return Registers.takeError();
  WindowsDriverState::EntryContext Entry{
      InitialRegisters, std::move(*Registers), InitialFrame,   {}, Stack.Base,
      Stack.Size,       std::nullopt,          EntryInvocation};
  if (auto E = read(InitialRegisters.at(CPURegister::X64SP)[0], Entry.Frame))
    return std::move(E);
  if (IncludeBacking) {
    Entry.StackBytes.emplace(Stack.Size);
    if (auto E = read(Stack.Base, *Entry.StackBytes))
      return std::move(E);
  }
  return Entry;
}
llvm::Expected<std::shared_ptr<const ProcessRuntimeState>>
DriverObservation::runtimeState(bool IncludeBacking) {
  auto Entry = entryContext(IncludeBacking);
  if (!Entry)
    return Entry.takeError();
  auto State = Kernel.captureUnpackRuntime(IncludeBacking);
  if (!State)
    return State.takeError();
  auto Relocations = imageRelocations();
  if (!Relocations)
    return Relocations.takeError();
  if (!entryContextUnchanged(*Entry))
    State->Reasons.emplace_back("changed DriverEntry invocation context");
  else if (ExecutedPrefix)
    // Admission is not a proof that the prefix preserves arbitrary incoming
    // values or that the continuation establishes its live outputs. Even a
    // same-value write can discard a fresh caller's register or return status.
    State->Reasons.emplace_back(
        "DriverEntry continuation requires fresh-caller reconstruction");
  State->Entry = std::move(*Entry);
  State->Relocations = std::move(*Relocations);
  return std::shared_ptr<const ProcessRuntimeState>(
      std::make_shared<WindowsDriverState>(std::move(*State)));
}
llvm::Expected<WindowsDriverState::ImageRelocationState>
DriverObservation::imageRelocations() {
  if (RelocationWrites.size() != Image.Relocations.size())
    return llvm::createStringError(
        "driver relocation witnesses are incomplete");
  WindowsDriverState::ImageRelocationState State{
      Image.Base, Image.PreferredBase, {}, Image.FileHash};
  for (size_t I = 0; I < Image.Relocations.size(); ++I) {
    const auto &Field = Image.Relocations[I];
    const auto &Witness = RelocationWrites[I];
    auto Bytes = CPU.addressSpace()->pinBacking(Image.Base + Field.RVA, 8);
    bool SameMapping = false;
    if (Bytes)
      SameMapping = Bytes->describesSameBytes(Witness.Bytes);
    else
      llvm::consumeError(Bytes.takeError());
    auto Intact = Witness.Writes.intact();
    if (!Intact)
      return Intact.takeError();
    State.Fields.push_back(
        {Field.RVA, Field.LoadedValue, SameMapping && *Intact});
  }
  return State;
}
std::optional<uint64_t> DriverObservation::nativeCallCount() const {
  return Result.Calls.size();
}
llvm::Expected<uint32_t> DriverObservation::instructionSize(uint64_t Address) {
  return CPU.instructionSize(Address);
}
} // namespace neverd::emulation
