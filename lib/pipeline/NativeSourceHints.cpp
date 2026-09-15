#include "neverd/pipeline/NativeSourceHints.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {
bool integerCarrier(const TypeRef &Type) {
  return Type &&
         ((Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8)) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size == 8 && Type->Pointee));
}

bool sameScalar(const TypeRef &A, const TypeRef &B) {
  return integerCarrier(A) && integerCarrier(B) && A->Kind == B->Kind &&
         A->Size == B->Size && A->IsSigned == B->IsSigned;
}

// These are internal source parameters, not a guessed external convention.
// Use MedIR's observable entry-byte analysis and independent full-width native
// reads. Caller-saved input registers may subsequently become scratch values.
// Preserved context inputs additionally require no native writes to preserved
// non-frame registers: hidden outputs cannot acquire a scalar call contract.
// The complete source body and its callers still require validation.
std::vector<uint64_t> nativeEntryRegisters(const LowFunc *Low,
                                           const MedFunc &Med,
                                           const SourceFunctionTypeHint &Hint) {
  if (!Low || Low->Entry != Med.Entry || Low->Blocks.empty() ||
      Low->Blocks.size() > 16384)
    return {};
  const auto Observed = observedMedSourceEntryRegisters(Med, Hint);
  if (!Observed)
    return {};
  const auto &TRI = getTargetRegInfo(Hint.Architecture);
  auto IntegerRegister = [&](uint64_t Register) {
    return !TRI.isFrameOrLinkReg(Register) &&
           (Hint.Architecture == Arch::AArch64
                ? Register <= a64reg::X28 && Register % 8 == 0
                : TRI.isGeneralReg(Register));
  };
  const auto Preserved = TRI.callPreservedRanges(BinaryFormat::MachO);
  size_t Remaining = 262144;
  std::set<uint64_t> Reads;
  bool PreservedWrite = false;
  for (const auto &Block : Low->Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Remaining-- || Op.NumInputs > 6)
        return {};
      const auto &Output = Op.Output;
      if (Output.isReg() && Output.Size &&
          !(TRI.isFrameOrLinkReg(Output.Offset) && Output.Size <= 8))
        for (const auto &Range : Preserved)
          if (!TRI.isFrameOrLinkReg(Range.Offset) &&
              (Output.Offset <= Range.Offset
                   ? Range.Offset - Output.Offset < Output.Size
                   : Output.Offset - Range.Offset < Range.Bytes))
            PreservedWrite = true;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (!Remaining--)
          return {};
        const auto &Input = Op.Inputs[I];
        if (Input.isReg() && Input.Size == 8 && IntegerRegister(Input.Offset) &&
            Observed->count(Input.Offset))
          Reads.insert(Input.Offset);
      }
    }
  if (PreservedWrite)
    std::erase_if(Reads, [&](uint64_t Register) {
      return TRI.isCallPreserved(Register, 8);
    });
  return {Reads.begin(), Reads.end()};
}

// Typed two-word calls materialize their physical results through SUBBYTES.
// Authenticate the complete lowering prefix before treating those extracts
// as register definitions; ordinary SUBBYTES operations are only views.
bool completeCallResultPrefix(llvm::ArrayRef<MedOp> Ops, size_t Index,
                              Arch Architecture) {
  const auto &Call = Ops[Index];
  if (Ops.size() - Index < 3 || !Call.SourceCallHint ||
      Call.Output.Kind != MedVar::Temp || Call.Output.Id < 0 ||
      Call.Output.Size != 16)
    return false;
  const auto &Signature = Call.SourceCallHint->Signature;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (!Signature.HasExplicitABI || Signature.Architecture != Architecture ||
      !Signature.ReturnType || Signature.ReturnType->Size != 16 ||
      Signature.ReturnComponents.size() != 2 || TRI.IntReturnRegs.size() < 2)
    return false;
  for (unsigned I = 0; I < 2; ++I) {
    const auto &Location = Signature.ReturnComponents[I];
    const auto &Extract = Ops[Index + I + 1];
    if (Location.Kind != SourceABICarrierKind::IntegerRegister ||
        Location.RegisterOffset != TRI.IntReturnRegs[I] ||
        Location.ValueBytes != 8 || Extract.Opcode != NdOp::SUBBYTES ||
        Extract.NumInputs != 2 || Extract.Output.Kind != MedVar::Reg ||
        Extract.Output.RegOff != Location.RegisterOffset ||
        Extract.Output.Size != 8 || Extract.Inputs[0] != Call.Output ||
        Extract.Inputs[0].Size != 16 || !Extract.Inputs[1].isConst() ||
        Extract.Inputs[1].ConstVal != I * 8U)
      return false;
  }
  return true;
}

// This proves a defined machine carrier, not an original return declaration.
// An observed full-width parameter can supply the initial register value.
// PHIs merge physical state, and calls or partial writes invalidate it until
// a complete result is computed again.
bool definedReturnPaths(const MedFunc &Function, Arch Architecture,
                        uint16_t Width,
                        const std::optional<MedVar> &IncomingParameter) {
  const size_t Count = Function.Blocks.size();
  if (!Count || Count > 16384)
    return false;
  size_t Remaining = 262144;
  std::map<int, size_t> Index;
  std::optional<size_t> Entry;
  bool HasAddresses = false;
  for (size_t I = 0; I < Count; ++I) {
    const auto &Block = Function.Blocks[I];
    if (Block.Id < 0 || !Index.emplace(Block.Id, I).second)
      return false;
    HasAddresses |= Block.StartAddr != 0;
    if (Block.StartAddr == Function.Entry) {
      if (Entry)
        return false;
      Entry = I;
    }
  }
  if (!Entry && !HasAddresses && Index.count(0))
    Entry = Index.at(0);
  if (!Entry)
    return false;
  std::vector<std::set<size_t>> Preds(Count), Succs(Count);
  for (size_t I = 0; I < Count; ++I)
    for (int Successor : Function.Blocks[I].Succs) {
      const auto Found = Index.find(Successor);
      if (!Remaining-- || Found == Index.end() ||
          !Succs[I].insert(Found->second).second)
        return false;
      Preds[Found->second].insert(I);
    }
  for (size_t I = 0; I < Count; ++I) {
    std::set<size_t> Declared;
    for (int Predecessor : Function.Blocks[I].Preds) {
      const auto Found = Index.find(Predecessor);
      if (!Remaining-- || Found == Index.end() ||
          !Declared.insert(Found->second).second)
        return false;
    }
    if (Declared != Preds[I])
      return false;
  }
  std::vector<bool> Reachable(Count);
  std::vector<size_t> Visit{*Entry};
  Reachable[*Entry] = true;
  for (size_t I = 0; I < Visit.size(); ++I)
    for (size_t Successor : Succs[Visit[I]])
      if (!Reachable[Successor]) {
        Reachable[Successor] = true;
        Visit.push_back(Successor);
      }
  const auto &TRI = getTargetRegInfo(Architecture);
  // Nonconstant MedVar equality is kind/id/version. Version zero alone is
  // not an entry identity: an internal definition can receive that version.
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  std::set<ValueKey> Definitions, IncomingUses;
  auto Define = [&](const MedVar &Value) {
    if (IncomingParameter && Value.Size &&
        (Value.Kind == MedVar::Reg || Value.Kind == MedVar::Param))
      Definitions.emplace(Value.Kind, Value.Id, Value.SSAVer);
  };
  std::vector<std::optional<bool>> Transfer(Count);
  std::vector<std::pair<size_t, std::optional<bool>>> Returns;
  for (size_t I = 0; I < Count; ++I) {
    auto &Fact = Transfer[I];
    if (IncomingParameter)
      for (const auto &Phi : Function.Blocks[I].Phis) {
        if (!Remaining--)
          return false;
        Define(Phi.Output);
      }
    const auto &Ops = Function.Blocks[I].Ops;
    size_t CallResultEnd = 0;
    for (size_t OpIndex = 0; OpIndex < Ops.size(); ++OpIndex) {
      const auto &Op = Ops[OpIndex];
      if (!Remaining--)
        return false;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        Fact = false;
        if (Remaining < 2)
          return false;
        Remaining -= 2;
        CallResultEnd = completeCallResultPrefix(Ops, OpIndex, Architecture)
                            ? OpIndex + 3
                            : 0;
      }
      const bool Seed = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Output == Op.Inputs[0] &&
                        Op.Output.Size == Op.Inputs[0].Size;
      if (!Seed)
        Define(Op.Output);
      // Params alone do not prove that a generic ABI placeholder was read.
      // Only actual reachable uses of the entry version can supply this
      // initial fact; SSA seeds, return markers and PHIs cannot invent it.
      if (IncomingParameter && Reachable[I] && !Seed &&
          Op.Opcode != NdOp::RETURN)
        for (unsigned J = 0; J < Op.NumInputs; ++J) {
          if (!Remaining--)
            return false;
          const auto &Input = Op.Inputs[J];
          if (((Input.Kind == MedVar::Reg && Input.SSAVer == 0) ||
               (Input.Kind == MedVar::Param && Input == *IncomingParameter)) &&
              Input.RegOff == IncomingParameter->RegOff && Input.Size >= Width)
            IncomingUses.emplace(Input.Kind, Input.Id, Input.SSAVer);
        }
      const auto &Out = Op.Output;
      if (!Seed && (Op.Opcode != NdOp::SUBBYTES || OpIndex < CallResultEnd) &&
          Out.Kind == MedVar::Reg && Out.Size &&
          (Out.RegOff <= TRI.IntReturnReg
               ? TRI.IntReturnReg - Out.RegOff < Out.Size
               : Out.RegOff - TRI.IntReturnReg < Width)) {
        const bool StackRestore =
            Architecture == Arch::X64 && Op.Opcode == NdOp::LOAD &&
            Op.NumInputs == 1 && Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].RegOff == TRI.StackPointer;
        Fact = Out.RegOff == TRI.IntReturnReg && Out.Size >= Width &&
               !StackRestore;
      }
      if (Op.Opcode == NdOp::RETURN)
        Returns.emplace_back(I, Fact);
    }
  }
  const bool ObservedIncoming =
      std::any_of(IncomingUses.begin(), IncomingUses.end(),
                  [&](const ValueKey &Key) { return !Definitions.count(Key); });
  // Greatest fixed point of definite availability. Only inherited facts can
  // fall from true to false; a local complete write establishes its own fact.
  // Entry needs its observed parameter; disconnected components have no
  // initial result. Backedges must agree with the initial entry fact too.
  std::vector<bool> Outgoing(Count);
  std::deque<size_t> Unknown;
  for (size_t I = 0; I < Count; ++I) {
    Outgoing[I] = Transfer[I].value_or(
        Reachable[I] && (I == *Entry ? ObservedIncoming : !Preds[I].empty()));
    if (!Outgoing[I])
      Unknown.push_back(I);
  }
  while (!Unknown.empty()) {
    const size_t I = Unknown.front();
    Unknown.pop_front();
    for (size_t Successor : Succs[I]) {
      if (!Remaining--)
        return false;
      if (!Transfer[Successor] && Outgoing[Successor]) {
        Outgoing[Successor] = false;
        Unknown.push_back(Successor);
      }
    }
  }
  // Compute each meet once even when a malformed block has many returns.
  std::vector<bool> Incoming(Count);
  for (size_t I = 0; I < Count; ++I)
    Incoming[I] =
        Reachable[I] && (I == *Entry ? ObservedIncoming : !Preds[I].empty()) &&
        std::all_of(Preds[I].begin(), Preds[I].end(),
                    [&](size_t Predecessor) { return Outgoing[Predecessor]; });
  for (const auto &[I, Fact] : Returns)
    if (!Fact.value_or(Incoming[I]))
      return false;
  return !Returns.empty();
}
} // namespace

std::optional<SourceFunctionTypeHint>
inferNativeSourceTypeHint(const BinaryImage &Image, const MedFunc &Med,
                          const HighFunc &High,
                          const PipelineFunctionAudit &Audit,
                          std::string &Diagnostic, const LowFunc *Low) {
  Diagnostic.clear();
  auto Reject =
      [&](const char *Reason) -> std::optional<SourceFunctionTypeHint> {
    Diagnostic = Reason;
    return std::nullopt;
  };
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.isCodeAddress(Med.Entry) || Med.Entry != High.Entry ||
      Med.Entry != Audit.Entry)
    return Reject(
        "native source inference requires one linked Darwin function");
  if (Audit.Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit.HasLowIR || !Audit.HasMedIR || !Audit.MedIRVerified ||
      !Audit.DecodedInstructions ||
      Audit.DecodedInstructions != Audit.LiftedInstructions ||
      !Audit.DecodeFailures.empty() || !Audit.UnsupportedInstructions.empty() ||
      !Audit.TruncatedPaths.empty())
    return Reject("native source inference requires complete verified lifting");
  if (Med.SourceTypeHint || High.SourceTypeHint || Med.SourceParametersBound)
    return Reject("native function already has a source declaration");
  if (Med.IsVariadic || !Med.MultiReturn.empty() || Med.FPReturnViaX87 ||
      Med.DoesNotReturn || High.DoesNotReturn)
    return Reject("native function has a non-scalar or non-returning ABI");
  if (Med.Blocks.empty() || High.Body.empty() ||
      Med.Params.size() != Med.TypedParams.size() || Med.Params.size() > 64 ||
      Med.Params.size() != High.Params.size() ||
      !sameScalar(Med.ReturnType, High.ReturnType))
    return Reject("native scalar parameter or return types are incomplete");

  const auto &TRI = getTargetRegInfo(Image.Arch);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Image.Arch;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = Med.ReturnType;
  Hint.ReturnLocation.Kind = SourceABICarrierKind::IntegerRegister;
  Hint.ReturnLocation.RegisterOffset = TRI.IntReturnReg;
  Hint.ReturnLocation.ValueBytes = Hint.ReturnType->Size;
  std::set<uint64_t> ParameterRegisters;
  std::set<uint64_t> AuxiliaryRegisters;
  std::set<int> StackSlots;
  std::optional<MedVar> IncomingReturnParameter;
  for (size_t Index = 0; Index < Med.Params.size(); ++Index) {
    const auto &Parameter = Med.Params[Index];
    // Generic ABI recovery fills unused register gaps with Id=-1. Such a
    // placeholder proves neither a type nor an argument; retain only observed
    // inputs, with their original physical register positions unchanged.
    if (Parameter.Id < 0)
      continue;
    const auto &Type = Med.TypedParams[Index].Type;
    if (Parameter.Kind != MedVar::Param || !integerCarrier(Type) ||
        Type->Size != Parameter.Size)
      return Reject("native parameter lacks a scalar machine carrier");
    SourceParameterTypeHint Source;
    Source.Name = "native_arg" + std::to_string(Hint.Parameters.size());
    Source.Type = Type;
    Source.Location.ValueBytes = Type->Size;
    if (Parameter.RegOff == kNoParamReg) {
      // This is the generic stack recovery contract, not an inferred C
      // parameter index. A narrow SUBBYTES use can represent several packed
      // arm64 values in one slot, so it requires a separate range analysis.
      const int RegisterCount = static_cast<int>(TRI.IntParamRegs.size());
      if (Parameter.Size != 8 || Parameter.Id < RegisterCount ||
          Parameter.Id >= RegisterCount + 512)
        return Reject("native stack parameter has no complete slot evidence");
      Source.Location.Kind = SourceABICarrierKind::Stack;
      Source.Location.EntryStackOffset =
          (Image.Arch == Arch::X64 ? 8 : 0) +
          int64_t(Parameter.Id - RegisterCount) * 8;
      if (!StackSlots.insert(Parameter.Id).second)
        return Reject("native stack parameters overlap");
    } else {
      if (!ParameterRegisters.insert(Parameter.RegOff).second)
        return Reject("native parameter register is ambiguous or non-integer");
      if (std::find(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end(),
                    Parameter.RegOff) == TRI.IntParamRegs.end()) {
        if (Parameter.Size != 8)
          return Reject("native auxiliary parameter requires a complete word");
        AuxiliaryRegisters.insert(Parameter.RegOff);
      }
      Source.Location.Kind = SourceABICarrierKind::IntegerRegister;
      Source.Location.RegisterOffset = Parameter.RegOff;
      if (Parameter.RegOff == TRI.IntReturnReg &&
          Parameter.Size >= Hint.ReturnType->Size)
        IncomingReturnParameter = Parameter;
    }
    Hint.Parameters.push_back(std::move(Source));
  }
  if (!Med.MutableStackParamHomes.empty()) {
    // Mutable homes are stored by parameter index, whereas ordinary stack
    // Params use ABI slot IDs. Do not combine these coordinate systems.
    return Reject("native mutable stack parameters require range recovery");
  }

  bool HasReturn = false;
  for (const auto &Block : Med.Blocks) {
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return Reject("native exception-dependent parameters are unsupported");
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::INTRINSIC)
        return Reject("native intrinsic requires explicit scalar ABI evidence");
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        std::string Error;
        if (!Op.SourceCallHint ||
            !validateSourceABI(Op.SourceCallHint->Signature, Error) ||
            Op.NumInputs != Op.SourceCallHint->Signature.Parameters.size() + 1)
          return Reject(
              "native function calls a target without a source binding");
      }
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const auto &Input = Op.Inputs[I];
        if (Input.Kind != MedVar::Param)
          continue;
        if (Input.RegOff != kNoParamReg) {
          if (!ParameterRegisters.count(Input.RegOff))
            return Reject("native body uses an unbound input register");
          continue;
        }
        if (!StackSlots.count(Input.Id) || Input.Size != 8 ||
            (Op.Opcode == NdOp::SUBBYTES &&
             (I != 0 || Op.NumInputs != 2 || !Op.Inputs[1].isConst() ||
              Op.Inputs[1].ConstVal != 0 || Op.Output.Size != 8)))
          return Reject("native stack slot is partial or has an unknown range");
      }
      HasReturn |= Op.Opcode == NdOp::RETURN;
    }
    for (const auto &Phi : Block.Phis)
      for (const auto &[Predecessor, Input] : Phi.Args)
        if (Input.Kind == MedVar::Param && Input.RegOff == kNoParamReg)
          return Reject("native stack parameter PHI requires range recovery");
  }
  if (!HasReturn)
    return Reject("native function has no machine return");
  if (!definedReturnPaths(Med, Image.Arch, Hint.ReturnType->Size,
                          IncomingReturnParameter))
    return Reject("native result has no complete defined carrier on every "
                  "return path");
  const auto PointerParameters = inferMedSourcePointerParameters(Med);
  size_t SourceIndex = 0;
  for (size_t Index = 0; Index < Med.Params.size(); ++Index) {
    if (Med.Params[Index].Id < 0)
      continue;
    auto &Parameter = Hint.Parameters[SourceIndex++];
    if (PointerParameters[Index] && Parameter.Type->Kind == NdTypeKind::Int)
      Parameter.Type = NdType::makePtr(NdType::makeVoid());
  }
  if (!validateSourceABI(Hint, Diagnostic))
    return std::nullopt;
  const auto EntryRegisters = nativeEntryRegisters(Low, Med, Hint);
  for (uint64_t Register : AuxiliaryRegisters)
    if (std::find(EntryRegisters.begin(), EntryRegisters.end(), Register) ==
        EntryRegisters.end())
      return Reject(
          "native auxiliary parameter lacks observed native input evidence");
  for (uint64_t Register : EntryRegisters)
    if (!ParameterRegisters.count(Register))
      Hint.Parameters.push_back(
          {"native_arg" + std::to_string(Hint.Parameters.size()),
           NdType::makeInt(8),
           {SourceABICarrierKind::IntegerRegister, Register, 0, 8}});
  if (!validateSourceABI(Hint, Diagnostic))
    return std::nullopt;
  return Hint;
}
} // namespace neverd
