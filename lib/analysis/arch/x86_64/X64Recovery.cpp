//===- X64Recovery.cpp - Native x64 recovery contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64Recovery.h"

#include "../../../lift/X86/X86CetEncoding.h"
#include "NativeStackControl.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <set>

namespace neverd::analysis::x64 {
llvm::Expected<RawEnvironmentPreparation>
validateRawEnvironmentDeclarations(const SpecializationOptions &Options) {
  RawEnvironmentPreparation Cost;
  if (!Options.hasRawEnvironment())
    return Cost;
  const auto Invalid = [](const char *Message) {
    return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
  };
  const auto Exhausted = [] {
    return llvm::createStringError(
        std::make_error_code(std::errc::value_too_large),
        "raw environment preparation budget exhausted");
  };
  // V1 reserves thirty-two bounded preparation passes (validation, owned copies
  // and serialization/hash) before examining any declaration payload. Scalar
  // option fields fit in the 1024-byte header; each record adds 128 bytes.
  // Payload lengths are inspected only after aggregate collection counts fit.
  constexpr uint64_t Passes = 32;
  uint64_t Records = 1;
  const uint64_t RecordLimit = Options.MaxImagePreparationRecords / Passes;
  for (size_t Count :
       {Options.X64CodeRegions.size(), Options.X64ImportBindings.size(),
        Options.X64TerminalImports.size(), Options.X64MachineExits.size(),
        Options.ControlRegisters.size(), Options.ControlFrameSlots.size(),
        Options.EntryConstants.size()}) {
    if (Records > RecordLimit || Count > RecordLimit - Records)
      return Exhausted();
    Records += Count;
  }
  // Pairwise interval and target checks have an explicit worst-case record
  // reservation too. Avoid an uncharged sort or implementation-dependent
  // comparator count; the small raw-domain interface uses bounded lists.
  if (Records > RecordLimit || Records > RecordLimit / Records)
    return Exhausted();
  const uint64_t ByteLimit = Options.MaxImagePreparationBytes / Passes;
  if (ByteLimit < 1024 || Records > (ByteLimit - 1024) / 128)
    return Exhausted();
  uint64_t Bytes = 1024 + Records * 128;
  const auto Add = [&](uint64_t Size) {
    if (Size > ByteLimit - Bytes)
      return false;
    Bytes += Size;
    return true;
  };
  for (const auto &R : Options.X64CodeRegions)
    if (!Add(R.Bytes.size()) || !Add(R.Evidence.size()))
      return Exhausted();
  for (const auto &B : Options.X64ImportBindings)
    if (!Add(B.Module.size()) || !Add(B.Symbol.size()) ||
        !Add(B.Evidence.size()))
      return Exhausted();
  for (const auto &B : Options.X64TerminalImports)
    if (!Add(B.Module.size()) || !Add(B.Symbol.size()))
      return Exhausted();
  Cost.Bytes = Bytes * Passes;
  Cost.Records = Records * Records * Passes;
  // Each primary identity lookup performs two binary searches in size_t-wide
  // indexes (named slots and section mappings): at most 64 comparisons each.
  // This pays the declaration/primary-index boundary independently of either
  // collection's size. Unique slots prevent repeated copying of one identity.
  const uint64_t ImportCount =
      Options.X64ImportBindings.size() + Options.X64TerminalImports.size();
  if (ImportCount > (Options.MaxImagePreparationRecords - Cost.Records) / 128)
    return Exhausted();
  Cost.Records += ImportCount * 128;

  if (!Options.ExplicitMachineState || !Options.NormalNonfaultingExecution ||
      !Options.X64CetDisabled ||
      Options.ByteOrder != llvm::endianness::little ||
      !Options.FrameBaseRegister ||
      Options.FrameBaseRegister->Offset != StackPointer ||
      Options.FrameBaseRegister->Bytes != 8 ||
      (Options.X64MachineExits.empty() &&
       !Options.RequireRestoredFrameAtReturn))
    return Invalid("raw environment requires explicit nonfaulting CET-disabled "
                   "x64 state and an explicit entry RSP contract");
  if (!Options.X64MachineExits.empty() &&
      (Options.RequireRestoredFrameAtReturn ||
       Options.ExternalStoresPreserveEntryReturnSlot ||
       !Options.X64TerminalImports.empty()))
    return Invalid("machine regions exclude ordinary return-slot and terminal "
                   "import contracts");
  for (size_t I = 0; I < Options.X64MachineExits.size(); ++I) {
    const auto &E = Options.X64MachineExits[I];
    if (E.Version != 1 || E.Address == InvalidVA ||
        (E.Kind != SpecializationX64MachineExitKind::Call &&
         E.Kind != SpecializationX64MachineExitKind::Jump &&
         E.Kind != SpecializationX64MachineExitKind::Return))
      return Invalid("invalid machine-exit declaration");
    for (size_t J = 0; J < I; ++J)
      if (Options.X64MachineExits[J].Address == E.Address)
        return Invalid("duplicate machine-exit declaration");
  }
  if (auto Error = validateX64TerminalImportDomain(Options))
    return std::move(Error);
  const auto Text = [](llvm::StringRef S, bool AllowSpace = false) {
    return !S.empty() && S.size() <= 4096 &&
           llvm::all_of(S, [&](unsigned char C) {
             return C >= (AllowSpace ? 32 : 33) && C <= 126;
           });
  };
  std::vector<std::pair<va_t, va_t>> Ranges;
  for (const auto &R : Options.X64CodeRegions) {
    if (R.Version != 1 || R.Bytes.empty() ||
        R.Bytes.size() > InvalidVA - R.Address || !Text(R.Evidence, true))
      return Invalid("invalid supplemental code region");
    Ranges.emplace_back(R.Address, R.Address + R.Bytes.size());
  }
  for (size_t I = 0; I < Ranges.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Ranges[I].first < Ranges[J].second &&
          Ranges[J].first < Ranges[I].second)
        return Invalid("overlapping supplemental code regions");
  const auto Contains = [&](va_t Target) {
    return llvm::any_of(Ranges, [&](const auto &R) {
      return Target >= R.first && Target < R.second;
    });
  };
  std::vector<std::pair<va_t, va_t>> Slots;
  for (const auto &B : Options.X64TerminalImports)
    Slots.emplace_back(B.Slot, B.Slot + 8);
  for (const auto &B : Options.X64ImportBindings) {
    if (B.Version != 1 || !B.Slot || B.Slot > InvalidVA - 8 ||
        !Text(B.Module) || !Text(B.Symbol) || !Text(B.Evidence, true) ||
        !Contains(B.Target))
      return Invalid("invalid stable named import binding");
    Slots.emplace_back(B.Slot, B.Slot + 8);
  }
  for (size_t I = 0; I < Slots.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Slots[I].first < Slots[J].second && Slots[J].first < Slots[I].second)
        return Invalid("overlapping stable or terminal import bindings");
  return Cost;
}

/// The x64 lifter uses these exact shapes to read and restore the system
/// portion of RFLAGS. The residual keeps both intrinsics. During analysis a
/// PUSHFQ snapshot is an unconstrained runtime value: this overapproximates
/// every architectural flag image without inventing a constant or an alias
/// fact. POPFQ has no modelled scalar output; later snapshots are fresh again.
bool runtimeFlagsIntrinsic(const LowOp &Op) {
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::Pushf)
    return Op.NumInputs == 1 && Op.Output.isTemp() && Op.Output.Size == 8;
  if (Id == Intrinsic::Popf)
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Op.Inputs[1].Size == 8;
  return false;
}

// Intel SDM Vol.2B, RDTSC: the low/high words go to EAX/EDX and both
// upper halves clear. The existing multi-output bundle keeps the runtime read;
// these scalar operations only connect its placeholders during raw analysis.
std::optional<RuntimeTimestampRead>
lowerRuntimeTimestampRead(const SpecializationInstruction &I, NdVar Sample) {
  const auto &B = I.Origin;
  if (I.NativeBytes.size() != 2 || I.NativeBytes[0] != 0x0f ||
      I.NativeBytes[1] != 0x31 || B.Size != 2 ||
      B.Mode != InstructionMode::Default || B.FirstOp != 0 || B.OpCount != 5 ||
      B.Control != LowInstructionControl::None ||
      B.ControlFlags != LowInstructionControlFlag::None || B.Immediate ||
      B.TargetMode != LowInstructionTargetMode::Preserve ||
      B.Address > InvalidVA - 2 ||
      I.Fallthrough != SpecializationCursor{B.Address + 2, B.Mode} ||
      I.IsNativeCall || I.TerminalImport ||
      I.NativeStackControl != SpecializationNativeStackControl::None ||
      I.ProfileProjection != InterpreterProfileProjection::None ||
      I.Ops.size() != 5 ||
      I.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      I.UndefinedEffects.OpCount != 5 || !I.UndefinedEffects.Effects.empty() ||
      I.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(I.Ops) ||
      !Sample.isTemp() || Sample.Size != 8 || Sample.Offset > InvalidVA - 7)
    return std::nullopt;
  for (size_t N = 0; N < I.Ops.size(); ++N)
    if (I.Ops[N].Addr != B.Address || I.Ops[N].Seq != static_cast<int>(N) ||
        I.Ops[N].MemoryOrdering != NdMemoryOrdering::None ||
        I.Ops[N].MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
  const auto &Read = I.Ops[0];
  if (Read.Opcode != NdOp::INTRINSIC || Read.NumInputs != 1 ||
      !Read.Inputs[0].isConst() || Read.Inputs[0].Size != 2 ||
      Read.Inputs[0].Offset != static_cast<uint64_t>(Intrinsic::Rdtsc) ||
      Read.Output != NdVar::reg(x86reg::RAX, 8))
    return std::nullopt;
  const auto Overlap = [](NdVar A, NdVar C) {
    return A.Offset <= C.Offset ? C.Offset - A.Offset < A.Size
                                : A.Offset - C.Offset < C.Size;
  };
  const std::array<NdVar, 2> Outputs{I.Ops[1].Inputs[0], I.Ops[2].Inputs[0]};
  for (unsigned N = 0; N != 2; ++N) {
    const auto Register = N ? x86reg::RDX : x86reg::RAX;
    const auto &Copy = I.Ops[1 + N];
    const auto &Clear = I.Ops[3 + N];
    const auto Value = Outputs[N];
    if (Copy.Opcode != NdOp::COPY || Copy.NumInputs != 1 ||
        Copy.Output != NdVar::reg(Register, 4) || !Value.isTemp() ||
        Value.Size != 4 || Value.Offset > InvalidVA - 3 ||
        Overlap(Value, Sample) || Clear.Opcode != NdOp::INT_ZEXT ||
        Clear.NumInputs != 1 || Clear.Output != NdVar::reg(Register, 8) ||
        Clear.Inputs[0] != NdVar::reg(Register, 4))
      return std::nullopt;
  }
  if (Overlap(Outputs[0], Outputs[1]))
    return std::nullopt;
  RuntimeTimestampRead Result;
  Result.PlaceholderOutputs = Outputs;
  for (unsigned N = 0; N != Result.Ops.size(); ++N) {
    auto &Op = Result.Ops[N];
    Op.Addr = B.Address;
    Op.Seq = N;
    Op.Opcode = N ? NdOp::SUBBYTES : NdOp::COPY;
    Op.Output = N ? Outputs[N - 1] : Read.Output;
    Op.addInput(Sample);
    if (N)
      Op.addInput(NdVar::scalar(4 * (N - 1), 8));
  }
  return Result;
}

// A retained trap has exact semantics even though its exception-state outputs
// have no undefined-effect audit. It may only be collected, never executed in
// this nonfaulting proof. Do not turn its Missing sidecar into Complete.
bool isRetainedNativeTrap(const SpecializationInstruction &Insn) {
  if (Insn.Origin.Control != LowInstructionControl::Terminator ||
      Insn.Origin.Immediate || Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.ProfileProjection != InterpreterProfileProjection::None ||
      Insn.Ops.size() != 1 || !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.Opcode != NdOp::INTRINSIC || Op.Output.Size || Op.NumInputs != 1 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  auto Flags = LowInstructionControlFlag::Terminator;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Int3))
    Flags |= LowInstructionControlFlag::Resumable;
  else if (Op.Inputs[0].Offset != static_cast<uint64_t>(Intrinsic::Ud2))
    return false;
  return Insn.Origin.ControlFlags == Flags;
}

// Intel SDM: disabled shadow stacks make RDSSPD/RDSSPQ a NOP, including no
// 32-bit destination zero-extension; INCSSPD/INCSSPQ instead raise #UD. Check
// the provider's classification against canonical bytes and exact operations.
// Missing coverage stays Missing. No other CET instruction is authorized.
bool isCetDisabledProjection(const SpecializationInstruction &Insn) {
  const bool Read = Insn.ProfileProjection ==
                    InterpreterProfileProjection::CetDisabledReadShadowStackV1;
  const bool Trap =
      Insn.ProfileProjection ==
      InterpreterProfileProjection::CetDisabledIncrementShadowStackTrapV1;
  if ((!Read && !Trap) || Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.Origin.Control != (Trap ? LowInstructionControl::Terminator
                                   : LowInstructionControl::None) ||
      Insn.Origin.ControlFlags != (Trap ? LowInstructionControlFlag::Terminator
                                        : LowInstructionControlFlag::None) ||
      Insn.Origin.Immediate || Insn.Ops.size() != 1 ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  // Preserve the strict lifter's opaque intrinsic result. It is never
  // executed: this profile faults before any architectural state update.
  if (Read ? (Op.Opcode != NdOp::NOP || Op.Output.Size || Op.NumInputs)
           : (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 1 ||
              Op.Output != NdVar::reg(x86reg::RAX, 8) ||
              !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
              Op.Inputs[0].Offset !=
                  static_cast<uint64_t>(Intrinsic::CetIncSsp)))
    return false;
  return x86cet::canonicalEncoding(Insn.NativeBytes,
                                   Read ? x86cet::Operation::Read
                                        : x86cet::Operation::Increment);
}

bool bindCetDisabledPreservedState(SpecializationInstruction &Insn) {
  Insn.PreservedState = {};
  if (Insn.ProfileProjection !=
          InterpreterProfileProjection::CetDisabledReadShadowStackV1 ||
      !isCetDisabledProjection(Insn))
    return false;
  auto &F = Insn.PreservedState;
  F.Audit = LowPreservedStateAudit::CetDisabledReadShadowStackV1;
  F.StateSet = LowPreservedStateSet::LegacyIntegerOpaqueV1;
  F.SemanticsVersion = 1;
  F.Architecture = Arch::X64;
  F.Mode = Insn.Origin.Mode;
  F.Address = Insn.Origin.Address;
  F.Size = Insn.Origin.Size;
  F.OpCount = Insn.Ops.size();
  F.NativeBytesDigest = llvm::toHex(llvm::SHA256::hash(Insn.NativeBytes), true);
  F.OperationDigest = lowUndefinedOperationDigest(Insn.Ops);
  if (!matchesLowPreservedState(F, Insn.Origin, Insn.NativeBytes, Insn.Ops)) {
    F = {};
    return false;
  }
  return true;
}

} // namespace neverd::analysis::x64
