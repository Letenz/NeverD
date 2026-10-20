//===- MedStackAlignment.cpp - Proven entry-stack alignment -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedStackAlignment.h"

#include "../../../safety/StackSlotFlow.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/X86RegistrationFrame.h"

#include <cstdint>
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

struct AlignmentResult {
  unsigned BaseIndex = 0;
  uint64_t Remainder = 0;
  int64_t Offset = 0;
};

struct AlignmentMask {
  unsigned BaseIndex = 0;
  uint64_t Alignment = 0;
};

std::optional<AlignmentMask> alignmentMask(const MedOp &Op,
                                           unsigned PointerSize) {
  if ((PointerSize != 4 && PointerSize != 8) || Op.Opcode != NdOp::INT_AND ||
      Op.NumInputs != 2 || Op.Output.Size != PointerSize ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.IntrinsicOutputs.empty())
    return std::nullopt;
  for (unsigned I = 0; I != 2; ++I) {
    const MedVar &Base = Op.Inputs[I];
    const MedVar &Mask = Op.Inputs[I ^ 1u];
    if (Base.isConst() || Base.Size != PointerSize || !Mask.isConst() ||
        Mask.Size != PointerSize || isAddressProvenance(Mask.Provenance))
      continue;
    const uint64_t WidthMask =
        PointerSize == 4 ? uint64_t{UINT32_MAX} : UINT64_MAX;
    if (Mask.ConstVal == 0 || (Mask.ConstVal & ~WidthMask))
      continue;
    const uint64_t Cleared = ~Mask.ConstVal & WidthMask;
    const uint64_t Alignment = Cleared + 1;
    if (Cleared && Alignment && (Alignment & (Alignment - 1)) == 0)
      return AlignmentMask{I, Alignment};
  }
  return std::nullopt;
}

class StackOffsetProof {
public:
  /// \p FollowJoins also takes a join whose incoming offsets agree, and an
  /// entry stack pointer no operation defines, which SSA makes the incoming
  /// value.  The alignment rewrite keeps the stricter proof.
  StackOffsetProof(const MedFunc &Func, Arch Architecture, BinaryFormat Format,
                   StackEntryKind Entry, bool FollowJoins = false)
      : Func(Func), TRI(getTargetRegInfo(Architecture)),
        Architecture(Architecture), Format(Format), Entry(Entry),
        FollowJoins(FollowJoins) {
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis)
        if (!FollowJoins || !Joins.emplace(key(Phi.Output), &Phi).second)
          Ambiguous.insert(key(Phi.Output));
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.isConst() || Op.Output.Size == 0)
          continue;
        if (!Definitions.emplace(key(Op.Output), &Op).second ||
            Joins.count(key(Op.Output)))
          Ambiguous.insert(key(Op.Output));
      }
    }
  }

  std::optional<AlignmentResult> alignment(const MedOp &Op,
                                           unsigned Depth = 0) {
    if (Depth == 0)
      Steps = 0;
    const auto Mask = alignmentMask(Op, TRI.PointerSize);
    if (!Mask || Mask->Alignment >
                     guaranteedEntryStackAlignment(Architecture, Format, Entry))
      return std::nullopt;
    auto Offset = resolve(Op.Inputs[Mask->BaseIndex], Depth + 1);
    if (!Offset)
      return std::nullopt;
    const uint64_t Remainder =
        (uint64_t(*Offset) +
         syntheticEntryStackResidue(Architecture, Format, Entry)) &
        (Mask->Alignment - 1);
    auto Aligned = safety::detail::checkedStackOffset(
        *Offset, static_cast<int64_t>(Remainder), true);
    if (!Aligned || !fitsPointerOffset(*Aligned))
      return std::nullopt;
    return AlignmentResult{Mask->BaseIndex, Remainder, *Aligned};
  }

  bool realignmentRoot(const MedOp &Op,
                       const std::set<const MedOp *> &EarlierEntryOps) {
    const auto Mask = alignmentMask(Op, TRI.PointerSize);
    const Key K = key(Op.Output);
    if (!Mask || Op.Output.Kind != MedVar::Reg ||
        Op.Output.TheArch != Architecture ||
        Op.Output.RegOff != TRI.StackPointer ||
        Op.RegistrationRoot != MedOp::RegistrationRootKind::None ||
        Ambiguous.count(K) || Definitions.find(K) == Definitions.end() ||
        Definitions.at(K) != &Op)
      return false;
    AllowedDefinitions = &EarlierEntryOps;
    return offset(Op.Inputs[Mask->BaseIndex]).has_value();
  }

  /// The offset of \p Value from the authenticated entry stack pointer.
  std::optional<int64_t> offset(const MedVar &Value) {
    Steps = 0;
    return resolve(Value, 0);
  }

private:
  /// The offset every way into \p Phi brings.  A way back through the join
  /// itself is checked against the offset the other ways agree on.
  std::optional<int64_t> join(const PhiNode &Phi, unsigned Depth) {
    std::optional<int64_t> Agreed;
    std::vector<const MedVar *> Cyclic;
    for (const auto &[Pred, Arg] : Phi.Args) {
      (void)Pred;
      const bool OuterCycle = CycleHit;
      CycleHit = false;
      const std::optional<int64_t> Offset = resolve(Arg, Depth + 1);
      const bool ThroughCycle = !Offset && CycleHit;
      CycleHit = OuterCycle;
      if (ThroughCycle) {
        Cyclic.push_back(&Arg);
        continue;
      }
      if (!Offset || (Agreed && *Agreed != *Offset))
        return std::nullopt;
      Agreed = Offset;
    }
    if (!Agreed)
      return std::nullopt;
    if (Cyclic.empty())
      return Agreed;
    const Key K = key(Phi.Output);
    Active.erase(K);
    Assumed.emplace(K, *Agreed);
    bool Holds = true;
    for (const MedVar *Arg : Cyclic) {
      const std::optional<int64_t> Offset = resolve(*Arg, Depth + 1);
      if (!Offset || *Offset != *Agreed) {
        Holds = false;
        break;
      }
    }
    Assumed.erase(K);
    Active.insert(K);
    return Holds ? Agreed : std::nullopt;
  }

  bool fitsPointerOffset(int64_t Offset) const {
    return TRI.PointerSize == 8 ||
           Offset == static_cast<int64_t>(static_cast<int32_t>(Offset));
  }

  std::optional<int64_t> resolve(const MedVar &Value, unsigned Depth) {
    // Joins can share their inputs, and nothing is cached, so bound the work
    // of one query.
    if (++Steps > kMaxProofSteps)
      return std::nullopt;
    if (Depth > 64 || Value.Size != TRI.PointerSize ||
        (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp) ||
        (AllowedDefinitions && Value.TheArch != Architecture))
      return std::nullopt;
    const Key K = key(Value);
    if (Ambiguous.count(K))
      return std::nullopt;
    const auto Definition = Definitions.find(K);
    if (AllowedDefinitions &&
        (Definition == Definitions.end() ||
         !AllowedDefinitions->count(Definition->second) ||
         Definition->second->MemoryOrdering != NdMemoryOrdering::None ||
         Definition->second->MemoryAddressSpace !=
             NdMemoryAddressSpace::Default ||
         !Definition->second->IntrinsicOutputs.empty()))
      return std::nullopt;
    if (Value.Kind == MedVar::Reg && Value.RegOff == TRI.StackPointer &&
        (safety::detail::isAuthenticatedEntryRegisterLiveIn(Func, Value) ||
         (FollowJoins && Value.SSAVer == 0 && !Definitions.count(K))))
      return 0;
    if (auto It = Assumed.find(K); It != Assumed.end())
      return It->second;
    if (!Active.insert(K).second) {
      CycleHit = true;
      return std::nullopt;
    }
    if (auto Join = Joins.find(K); Join != Joins.end()) {
      std::optional<int64_t> Result = join(*Join->second, Depth);
      Active.erase(K);
      return Result;
    }
    const auto It = Definitions.find(K);
    std::optional<int64_t> Result;
    if (It != Definitions.end()) {
      const MedOp &Op = *It->second;
      if (sameOccurrence(Op.Output, Value) &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.IntrinsicOutputs.empty()) {
        if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None) {
          if (Architecture == Arch::X86 && Format == BinaryFormat::COFF) {
            const auto Coordinate = registrationRootFrameCoordinate(Func, Op);
            if (Coordinate && Coordinate->Alignment == 1)
              Result =
                  int64_t(Coordinate->EntryOffset) + Coordinate->AlignedOffset;
          }
        } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
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
  StackEntryKind Entry;
  bool FollowJoins;
  // A realignment root must be established by the exact entry prefix already
  // executed before its mask. Ordinary entry-offset queries also follow joins.
  const std::set<const MedOp *> *AllowedDefinitions = nullptr;
  std::map<Key, const MedOp *> Definitions;
  std::map<Key, const PhiNode *> Joins;
  std::set<Key> Ambiguous;
  std::set<Key> Active;
  std::map<Key, int64_t> Assumed;
  bool CycleHit = false;
  static constexpr size_t kMaxProofSteps = 1 << 16;
  size_t Steps = 0;
};

} // namespace

std::vector<MedVar> entryStackRealignmentRoots(const MedFunc &Func,
                                               Arch Architecture,
                                               BinaryFormat Format) {
  const MedBlock *Entry = nullptr;
  for (const auto &Block : Func.Blocks)
    if (Block.StartAddr == Func.Entry) {
      if (Entry)
        return {};
      Entry = &Block;
    }
  if (!Entry || !Entry->Preds.empty() || !Entry->ExceptionalPreds.empty())
    return {};
  for (const auto &Block : Func.Blocks) {
    for (int Successor : Block.Succs)
      if (Successor == Entry->Id)
        return {};
    for (const auto &Successor : Block.ExceptionalSuccs)
      if (Successor.BlockId == Entry->Id)
        return {};
  }
  StackOffsetProof Proof(Func, Architecture, Format, StackEntryKind::Call);
  std::vector<MedVar> Roots;
  std::set<const MedOp *> EarlierEntryOps;
  for (const auto &Op : Entry->Ops) {
    if (Proof.realignmentRoot(Op, EarlierEntryOps))
      Roots.push_back(Op.Output);
    EarlierEntryOps.insert(&Op);
  }
  return Roots;
}

StackEntryKind functionEntryKind(const MedFunc &Func, StackEntryKind AtEntry) {
  if (AtEntry != StackEntryKind::ProcessEntry)
    return AtEntry;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN)
        return StackEntryKind::Call;
  return StackEntryKind::ProcessEntry;
}

std::optional<int64_t> entryStackOffset(const MedFunc &Func,
                                        const MedVar &Value, Arch Architecture,
                                        BinaryFormat Format,
                                        StackEntryKind Entry) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return std::nullopt;
  return StackOffsetProof(Func, Architecture, Format, Entry,
                          /*FollowJoins=*/true)
      .offset(Value);
}

std::vector<std::optional<int64_t>>
entryStackOffsets(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                  Arch Architecture, BinaryFormat Format,
                  StackEntryKind Entry) {
  std::vector<std::optional<int64_t>> Offsets(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Offsets;
  StackOffsetProof Proof(Func, Architecture, Format, Entry,
                         /*FollowJoins=*/true);
  for (size_t I = 0; I < Values.size(); ++I)
    Offsets[I] = Proof.offset(Values[I]);
  return Offsets;
}

void simplifyProvenStackAlignment(MedFunc &Func, Arch Architecture,
                                  BinaryFormat Format, StackEntryKind Entry) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return;
  StackOffsetProof Proof(Func, Architecture, Format, Entry);
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
