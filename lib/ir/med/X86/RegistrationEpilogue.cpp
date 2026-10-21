//===- RegistrationEpilogue.cpp - Physical C++ exit projection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationEpilogue.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/lift/LiftCommon.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"

#include <algorithm>
#include <map>

namespace neverd::x86_registration {
namespace {

std::optional<std::pair<std::vector<LowOp>, uint64_t>>
expandExit(const BinaryImage &Image, const LowFunc &Source,
           const LowBlock &Block, uint64_t TempId) {
  // The checked 20-byte helper cannot need 512 temporary slots. Reserve the
  // whole bounded expansion before appending any effect, including when many
  // separate exits follow source temporaries close to the storage limit.
  if (TempId > (UINT64_MAX - TmpBase) / TmpStride - 512)
    return std::nullopt;
  if (Block.Ops.empty() || Block.InstructionBoundaries.empty())
    return std::nullopt;
  const auto &Op = Block.Ops.back();
  const auto &Boundary = Block.InstructionBoundaries.back();
  if (Op.Opcode != NdOp::CALL ||
      std::count_if(Source.RegistrationStates->ChainAccesses.begin(),
                    Source.RegistrationStates->ChainAccesses.end(),
                    [&](const auto &Access) {
                      return Access.Address == Op.Addr &&
                             Access.EndAddress == Block.EndAddr &&
                             Access.OpSeq == Op.Seq &&
                             Access.AccessKind ==
                                 RegistrationChainAccess::Kind::Remove;
                    }) != 1)
    return std::nullopt;
  const auto Call = coff_loader::getCheckedX86CxxEpilogueCall(
      Image, *Source.ExceptionMetadata, Op.Addr);
  // Guarded exits need their observable checker and failure path preserved.
  // A source cookie receipt alone cannot replace that invocation with success.
  if (!Call || Call->CookieCheck || Call->Caller.End != Block.EndAddr ||
      Boundary.Address != Op.Addr || Boundary.Size != 5 ||
      Boundary.FirstOp != Block.Ops.size() - 1 || Boundary.OpCount != 1 ||
      Boundary.Mode != InstructionMode::Default ||
      Boundary.TargetMode != LowInstructionTargetMode::Preserve ||
      Boundary.Control != LowInstructionControl::Call ||
      Boundary.ControlFlags != LowInstructionControlFlag::Call ||
      Boundary.Immediate != Call->Helper.Begin)
    return std::nullopt;
  const auto Root = Source.OrdinaryModuleAnalysisRoots.upper_bound(Op.Addr);
  if (Root != Source.OrdinaryModuleAnalysisRoots.end() &&
      *Root < Call->Caller.End)
    return std::nullopt;
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  Dec.setStrict(true);
  auto Lift = [&](va_t Address, va_t End,
                  std::vector<LowOp> &Ops) -> std::optional<uint16_t> {
    const auto *Bytes = Image.readVA(Address, End - Address);
    DecodedInsn Insn;
    if (!Bytes || !Dec.decodeOne(Bytes, End - Address, Address, Insn))
      return std::nullopt;
    const auto Relocations = instructionRelocations(Image, Insn);
    try {
      Dec.liftToLow(Insn, Ops, Relocations.Addresses, Relocations.Scalars);
    } catch (const UnliftedInstruction &) {
      return std::nullopt;
    }
    return Insn.Size;
  };
  std::vector<LowOp> Caller;
  if (Lift(Op.Addr, Call->Caller.End, Caller) != 5 || Caller.size() != 1 ||
      Caller.front() != Op)
    return std::nullopt;

  std::vector<LowOp> Ops;
  LiftStateBase S(Op.Addr, 5, Ops);
  S.TmpId = TempId;
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  S.emit(NdOp::INT_SUB, SP, {SP, NdVar::scalar(4, 4)});
  S.emit(NdOp::STORE, {},
         {SP, NdVar::codeAddress(Call->Caller.End, 4, Source.Entry)});
  va_t Address = Call->Helper.Begin;
  bool Returned = false;
  while (Address < Call->Helper.End) {
    std::vector<LowOp> Body;
    const auto Size = Lift(Address, Call->Helper.End, Body);
    if (!Size)
      return std::nullopt;
    Address += *Size;
    if (Body.size() == 1 && Body.front().Opcode == NdOp::RETURN &&
        Address == Call->Helper.End) {
      // The ordinary RET lift omits its physical PC pop, just as CALL omits
      // the push above. Keep both here when composing the checked helper.
      S.emit(NdOp::INT_ADD, SP, {SP, NdVar::scalar(4, 4)});
      Returned = true;
      break;
    }
    // Fresh scratch storage per decoded helper instruction must not clobber
    // the source body's temporaries or another expanded helper occurrence.
    std::map<uint64_t, NdVar> Temps;
    auto Remap = [&](NdVar &Value) {
      if (!Value.isTemp() || !Value.Size)
        return true;
      auto It = Temps.find(Value.Offset);
      if (It == Temps.end())
        It = Temps.emplace(Value.Offset, S.makeTemp(Value.Size)).first;
      if (It->second.Size != Value.Size)
        return false;
      Value = It->second;
      return true;
    };
    for (auto &Effect : Body) {
      if (Effect.Opcode == NdOp::RETURN || Effect.Opcode == NdOp::CALL ||
          Effect.Opcode == NdOp::INDIR_CALL || Effect.Opcode == NdOp::BRANCH ||
          Effect.Opcode == NdOp::COND_BR || Effect.Opcode == NdOp::INDIR_BR ||
          !Remap(Effect.Output))
        return std::nullopt;
      for (unsigned I = 0; I != Effect.NumInputs; ++I)
        if (!Remap(Effect.Inputs[I]))
          return std::nullopt;
      Effect.Addr = Op.Addr;
      Effect.Seq = S.Seq++;
      Ops.push_back(std::move(Effect));
    }
  }
  if (!Returned)
    return std::nullopt;
  return std::make_pair(std::move(Ops), S.TmpId);
}

} // namespace

void projectCxxEpilogues(const BinaryImage &Image, const LowFunc &Source,
                         LowFunc &Physical) {
  if (!Source.ExceptionMetadata || !Source.RegistrationStates ||
      !Source.RegistrationStates->ChainOperationsComplete ||
      !Source.RegistrationStates->CxxPrologue ||
      Physical.Blocks.size() != Source.Blocks.size())
    return;
  uint64_t Next = TmpBase;
  auto Reserve = [&](const NdVar &Value) {
    if (!Value.isTemp())
      return true;
    if (Value.Offset > UINT64_MAX - Value.Size - 4096)
      return false;
    Next = std::max(Next, Value.Offset + Value.Size);
    return true;
  };
  for (const auto &Block : Physical.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Reserve(Op.Output))
        return;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        if (!Reserve(Op.Inputs[I]))
          return;
    }
  uint64_t TempId = (Next - TmpBase + TmpStride - 1) / TmpStride;
  for (size_t I = 0; I != Source.Blocks.size(); ++I) {
    auto Exit = expandExit(Image, Source, Source.Blocks[I], TempId);
    if (!Exit)
      continue;
    auto &Block = Physical.Blocks[I];
    Block.Ops.pop_back();
    Block.Ops.insert(Block.Ops.end(), Exit->first.begin(), Exit->first.end());
    Block.InstructionBoundaries.back().OpCount = Exit->first.size();
    TempId = Exit->second;
  }
}

} // namespace neverd::x86_registration
