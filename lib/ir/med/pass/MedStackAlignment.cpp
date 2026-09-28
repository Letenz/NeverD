//===- MedStackAlignment.cpp - Proven entry-stack alignment -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedStackAlignment.h"

#include "../../../safety/StackSlotFlow.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/TargetRegInfo.h"

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
namespace {

using Key = std::tuple<MedVar::VarKind, int, int>;

Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

bool sameOccurrence(const MedVar &A, const MedVar &B) {
  return key(A) == key(B) && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag &&
         (A.Kind != MedVar::Reg || A.RegOff == B.RegOff);
}

// ARM AAPCS requires an eight-byte-aligned public interface stack.  The
// source emitter's larger synthetic buffer alignment is not itself an ABI
// guarantee, so never use it to erase a real low bit of a machine value.
constexpr uint64_t guaranteedEntryAlignment(Arch Architecture,
                                            BinaryFormat Format) {
  if (Architecture == Arch::ARM)
    return 8;
  if (Architecture == Arch::AArch64 || Architecture == Arch::X64)
    return 16;
  if (Architecture == Arch::X86 && Format == BinaryFormat::MachO)
    return 16;
  return 4;
}

struct AlignmentResult {
  unsigned BaseIndex = 0;
  uint64_t Remainder = 0;
  int64_t Offset = 0;
};

class StackOffsetProof {
public:
  StackOffsetProof(const MedFunc &Func, Arch Architecture, BinaryFormat Format)
      : Func(Func), TRI(getTargetRegInfo(Architecture)),
        Architecture(Architecture), Format(Format) {
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        Ambiguous.insert(key(Phi.Output));
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.isConst() || Op.Output.Size == 0)
          continue;
        if (!Definitions.emplace(key(Op.Output), &Op).second)
          Ambiguous.insert(key(Op.Output));
      }
    }
  }

  std::optional<AlignmentResult> alignment(const MedOp &Op,
                                           unsigned Depth = 0) {
    if (Op.Opcode != NdOp::INT_AND || Op.NumInputs != 2 ||
        Op.Output.Size != TRI.PointerSize ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Op.IntrinsicOutputs.empty())
      return std::nullopt;
    for (unsigned BaseIndex = 0; BaseIndex != 2; ++BaseIndex) {
      const MedVar &Base = Op.Inputs[BaseIndex];
      const MedVar &Mask = Op.Inputs[1 - BaseIndex];
      if (Base.Size != TRI.PointerSize || Mask.Size != TRI.PointerSize ||
          !Mask.isConst() || isAddressProvenance(Mask.Provenance))
        continue;
      const uint64_t WidthMask = TRI.PointerSize == 4
                                     ? uint64_t{UINT32_MAX}
                                     : std::numeric_limits<uint64_t>::max();
      if ((Mask.ConstVal & ~WidthMask) != 0)
        continue;
      const uint64_t Cleared = ~Mask.ConstVal & WidthMask;
      const uint64_t Alignment = Cleared + 1;
      if (!Cleared || !Alignment ||
          Alignment > guaranteedEntryAlignment(Architecture, Format) ||
          (Alignment & (Alignment - 1)) != 0)
        continue;
      auto Offset = resolve(Base, Depth + 1);
      if (!Offset)
        continue;
      const uint64_t Remainder =
          (uint64_t(*Offset) +
           syntheticEntryStackResidue(Architecture, Format)) &
          (Alignment - 1);
      auto Aligned = safety::detail::checkedStackOffset(
          *Offset, static_cast<int64_t>(Remainder), true);
      if (!Aligned || !fitsPointerOffset(*Aligned))
        continue;
      return AlignmentResult{BaseIndex, Remainder, *Aligned};
    }
    return std::nullopt;
  }

private:
  bool fitsPointerOffset(int64_t Offset) const {
    return TRI.PointerSize == 8 ||
           Offset == static_cast<int64_t>(static_cast<int32_t>(Offset));
  }

  std::optional<int64_t> resolve(const MedVar &Value, unsigned Depth) {
    if (Depth > 64 || Value.Size != TRI.PointerSize ||
        (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp))
      return std::nullopt;
    const Key K = key(Value);
    if (Ambiguous.count(K))
      return std::nullopt;
    if (Value.Kind == MedVar::Reg && Value.RegOff == TRI.StackPointer &&
        safety::detail::isAuthenticatedEntryRegisterLiveIn(Func, Value))
      return 0;
    if (!Active.insert(K).second)
      return std::nullopt;
    const auto It = Definitions.find(K);
    std::optional<int64_t> Result;
    if (It != Definitions.end()) {
      const MedOp &Op = *It->second;
      if (sameOccurrence(Op.Output, Value) &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.IntrinsicOutputs.empty()) {
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
          Result = resolve(Op.Inputs[0], Depth + 1);
        else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
                 Op.NumInputs == 2) {
          unsigned BaseIndex =
              Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst() ? 1 : 0;
          const MedVar &Base = Op.Inputs[BaseIndex];
          const MedVar &Delta = Op.Inputs[1 - BaseIndex];
          if (Delta.Size == TRI.PointerSize) {
            auto Signed = safety::detail::signedStackConstant(Delta);
            auto BaseOffset = resolve(Base, Depth + 1);
            if (Signed && BaseOffset)
              Result = safety::detail::checkedStackOffset(
                  *BaseOffset, *Signed, Op.Opcode == NdOp::INT_SUB);
          }
        } else if (Op.Opcode == NdOp::INT_AND) {
          if (auto Aligned = alignment(Op, Depth + 1))
            Result = Aligned->Offset;
        }
      }
    }
    Active.erase(K);
    if (Result && !fitsPointerOffset(*Result))
      Result.reset();
    return Result;
  }

  const MedFunc &Func;
  const TargetRegInfo &TRI;
  Arch Architecture;
  BinaryFormat Format;
  std::map<Key, const MedOp *> Definitions;
  std::set<Key> Ambiguous;
  std::set<Key> Active;
};

} // namespace

void simplifyProvenStackAlignment(MedFunc &Func, Arch Architecture,
                                  BinaryFormat Format) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return;
  StackOffsetProof Proof(Func, Architecture, Format);
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if (const auto Aligned = Proof.alignment(Op)) {
        const MedVar Base = Op.Inputs[Aligned->BaseIndex];
        Op.Inputs[0] = Base;
        if (Aligned->Remainder == 0) {
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
        } else {
          Op.Opcode = NdOp::INT_SUB;
          Op.Inputs[1] = MedVar::makeConst(Aligned->Remainder, TRI.PointerSize,
                                           ConstantAddressProvenance::Scalar);
          Op.NumInputs = 2;
        }
      }
}

} // namespace neverd
