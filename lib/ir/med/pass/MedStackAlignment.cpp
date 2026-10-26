//===- MedStackAlignment.cpp - Proven entry-stack alignment -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Entry-stack offset proofs and alignment rewrites.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedStackAlignment.h"

#include "../../../safety/StackSlotFlow.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedProvenStackOffsets.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SaveAndRestore.h"

#include <algorithm>
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

unsigned definitionCapacity(const MedFunc &Func) {
  unsigned Count = 0;
  for (const MedBlock &Block : Func.Blocks) {
    Count += Block.Phis.size();
    for (const MedOp &Op : Block.Ops)
      if (!Op.Output.isConst() && Op.Output.Size != 0)
        ++Count;
  }
  return Count;
}

class StackOffsetProof {
  struct CompletedProof {
    MedVar Occurrence;
    int64_t Offset;
    size_t Steps;
    unsigned Depth;
  };

  struct ValueDefinition {
    const MedOp *Op = nullptr;
    const PhiNode *Join = nullptr;
    bool Ambiguous = false;
    size_t ActiveContext = 0;
    bool EntryChecked = false;
    bool EntryLiveIn = false;
    std::optional<int64_t> ProvenOffset;
  };

public:
  /// \p FollowJoins also takes a join whose incoming offsets agree, and an
  /// entry stack pointer no operation defines, which SSA makes the incoming
  /// value.  The alignment rewrite keeps the stricter proof.
  StackOffsetProof(const MedFunc &Func, Arch Architecture, BinaryFormat Format,
                   StackEntryKind Entry, bool FollowJoins = false,
                   llvm::ArrayRef<ProvenStackOffset> Seeds = {},
                   bool OnlySeededRoots = false)
      : Func(Func), TRI(getTargetRegInfo(Architecture)),
        Architecture(Architecture), Format(Format), Entry(Entry),
        FollowJoins(FollowJoins), OnlySeededRoots(OnlySeededRoots),
        Definitions(definitionCapacity(Func)) {
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis) {
        auto &Definition = Definitions[key(Phi.Output)];
        if (!FollowJoins || Definition.Join)
          Definition.Ambiguous = true;
        else
          Definition.Join = &Phi;
      }
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.isConst() || Op.Output.Size == 0)
          continue;
        auto &Definition = Definitions[key(Op.Output)];
        if (Definition.Op || Definition.Join)
          Definition.Ambiguous = true;
        if (!Definition.Op)
          Definition.Op = &Op;
      }
    }
    for (const auto &Seed : Seeds) {
      if (!Seed.Definition)
        continue;
      const auto Found = Definitions.find(key(Seed.Definition->Output));
      if (Found == Definitions.end() || Found->second.Ambiguous ||
          Found->second.Op != Seed.Definition || Seed.Definition->Dead ||
          Seed.Definition->Output.Size != TRI.PointerSize ||
          !fitsPointerOffset(Seed.Offset))
        continue;
      auto &Definition = Found->second;
      if (Definition.ProvenOffset && Definition.ProvenOffset != Seed.Offset)
        Definition.Ambiguous = true;
      else
        Definition.ProvenOffset = Seed.Offset;
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
    if (!Mask || Op.Output.Kind != MedVar::Reg ||
        Op.Output.TheArch != Architecture ||
        Op.Output.RegOff != TRI.StackPointer ||
        Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
      return false;
    const auto Definition = Definitions.find(key(Op.Output));
    if (Definition == Definitions.end() || Definition->second.Ambiguous ||
        Definition->second.Op != &Op)
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
  bool authenticatedEntryLiveIn(const MedVar &Value,
                                ValueDefinition *Definition) {
    // Alignment rewrites can change the entry COPY prefix. Only read-only
    // offset queries reuse authentication, and only for the exact occurrence.
    if (!FollowJoins)
      return safety::detail::isAuthenticatedEntryRegisterLiveIn(Func, Value);
    if (!Definition || !Definition->Op)
      return false;
    if (!Definition->EntryChecked) {
      Definition->EntryLiveIn =
          safety::detail::isAuthenticatedEntryRegisterLiveIn(
              Func, Definition->Op->Output);
      Definition->EntryChecked = true;
    }
    return Definition->EntryLiveIn && safety::detail::sameRegisterOccurrence(
                                          Definition->Op->Output, Value);
  }

  /// The offset every way into \p Phi brings.  A way back through the join
  /// itself is checked against the offset the other ways agree on.
  std::optional<int64_t> join(const PhiNode &Phi, unsigned Depth) {
    if (OnlySeededRoots && Phi.ExceptionalEntry)
      return std::nullopt;
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
    return Holds ? Agreed : std::nullopt;
  }

  bool fitsPointerOffset(int64_t Offset) const {
    return TRI.PointerSize == 8 ||
           Offset == static_cast<int64_t>(static_cast<int32_t>(Offset));
  }

  std::optional<int64_t> resolveJoin(const MedVar &Value,
                                     ValueDefinition &Definition,
                                     unsigned Depth) {
    // Shared joins branch into the same inputs repeatedly. Linear definitions
    // keep their ordinary traversal; only this read-only join proof looks up
    // completed work. The current resolve() has already charged its own step
    // and checked the occurrence, depth and active definition.
    const Key K = key(Value);
    if (auto It = Completed.find(K); It != Completed.end()) {
      const CompletedProof &Proof = It->second;
      if (sameOccurrence(Value, Proof.Occurrence) &&
          Proof.Steps <= kMaxProofSteps - Steps &&
          Proof.Depth <=
              ::neverd::limits::ir::kMedStackAlignmentResolveDepthLimit -
                  Depth) {
        // Charge every original input traversal, including repeated DAG edges.
        Steps += Proof.Steps;
        Deepest = std::max(Deepest, Depth + Proof.Depth);
        return Proof.Offset;
      }
    }
    const size_t Before = Steps;
    const size_t BeforeContext = ContextUses;
    const unsigned OuterDepth = Deepest;
    Deepest = Depth;
    const auto Result = join(*Definition.Join, Depth);
    const unsigned InnerDepth = Deepest;
    Deepest = std::max(OuterDepth, InnerDepth);
    // A successful traversal that never reads a loop assumption or an active
    // definition is independent of its ancestors. Failed or cyclic proofs
    // remain live queries, including joins that establish an assumed offset.
    if (Result && ContextUses == BeforeContext)
      Completed.insert_or_assign(
          K,
          CompletedProof{Value, *Result, Steps - Before, InnerDepth - Depth});
    return Result;
  }

  std::optional<int64_t> resolve(const MedVar &Value, unsigned Depth) {
    Deepest = std::max(Deepest, Depth);
    const bool WideAddress =
        FollowJoins && TRI.PointerSize == 4 && Value.Size == sizeof(uint64_t);
    // Keep the per-query work limit even when shared inputs reuse a proof.
    if (++Steps > kMaxProofSteps)
      return std::nullopt;
    if (Depth > ::neverd::limits::ir::kMedStackAlignmentResolveDepthLimit ||
        (Value.Size != TRI.PointerSize && !WideAddress) ||
        (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp) ||
        (AllowedDefinitions && Value.TheArch != Architecture))
      return std::nullopt;
    const Key K = key(Value);
    const auto It = Definitions.find(K);
    ValueDefinition *Definition =
        It == Definitions.end() ? nullptr : &It->second;
    // An induction assumption changes the query: an arithmetic definition
    // active outside that assumption must be checked again on the backedge.
    // The scoped marker still rejects cycles within the same context. Zero
    // means inactive; nested assumptions are pushed and popped in call order.
    const size_t Context = Assumed.size() + 1;
    if (Definition && Definition->Ambiguous)
      return std::nullopt;
    if (AllowedDefinitions &&
        (!Definition || !Definition->Op ||
         !AllowedDefinitions->count(Definition->Op) ||
         Definition->Op->MemoryOrdering != NdMemoryOrdering::None ||
         Definition->Op->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
         !Definition->Op->IntrinsicOutputs.empty()))
      return std::nullopt;
    if (WideAddress) {
      // The lifter carries native 32-bit memory addresses in a zero-extended
      // 64-bit address container. This preserves the target pointer; it is
      // not a truncation of a 64-bit SP or an eight-byte incoming SP seed.
      const MedOp *Op = Definition ? Definition->Op : nullptr;
      if (!Op || Definition->ActiveContext == Context ||
          !sameOccurrence(Op->Output, Value) || Op->Opcode != NdOp::INT_ZEXT ||
          Op->NumInputs != 1 || Op->Inputs[0].Size != TRI.PointerSize ||
          Op->MemoryOrdering != NdMemoryOrdering::None ||
          Op->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          !Op->IntrinsicOutputs.empty())
        return std::nullopt;
      llvm::SaveAndRestore Active(Definition->ActiveContext, Context);
      return resolve(Op->Inputs[0], Depth + 1);
    }
    if (!OnlySeededRoots && Value.Kind == MedVar::Reg &&
        Value.RegOff == TRI.StackPointer &&
        (authenticatedEntryLiveIn(Value, Definition) ||
         (FollowJoins && Value.SSAVer == 0 &&
          (!Definition || !Definition->Op))))
      return 0;
    if (Definition && Definition->ProvenOffset && Definition->Op &&
        sameOccurrence(Definition->Op->Output, Value))
      return Definition->ProvenOffset;
    if (auto It = Assumed.find(K); It != Assumed.end()) {
      ++ContextUses;
      return It->second;
    }
    if (!Definition)
      return std::nullopt;
    if (Definition->ActiveContext == Context) {
      ++ContextUses;
      CycleHit = true;
      return std::nullopt;
    }
    llvm::SaveAndRestore Active(Definition->ActiveContext, Context);
    if (Definition->Join)
      return resolveJoin(Value, *Definition, Depth);
    std::optional<int64_t> Result;
    if (Definition->Op) {
      const MedOp &Op = *Definition->Op;
      if (sameOccurrence(Op.Output, Value) &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.IntrinsicOutputs.empty()) {
        if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None) {
          if (!OnlySeededRoots && Architecture == Arch::X86 &&
              Format == BinaryFormat::COFF) {
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
          auto Signed = safety::detail::signedStackConstant(Delta);
          // ARM64 immediates can retain their encoded 32-bit carrier in a
          // pointer-width ADD/SUB. A nonnegative narrow constant has the same
          // value under either extension. Do not guess the extension of a
          // negative narrow carrier or admit a wider/truncated displacement.
          if (Signed && (Delta.Size == TRI.PointerSize ||
                         (Delta.Size < TRI.PointerSize && *Signed >= 0))) {
            auto BaseOffset = resolve(Base, Depth + 1);
            if (BaseOffset)
              Result = safety::detail::checkedStackOffset(
                  *BaseOffset, *Signed, Op.Opcode == NdOp::INT_SUB);
          }
        } else if (!OnlySeededRoots && Op.Opcode == NdOp::INT_AND) {
          if (auto Aligned = alignment(Op, Depth + 1))
            Result = Aligned->Offset;
        }
      }
    }
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
  bool OnlySeededRoots;
  // A realignment root must be established by the exact entry prefix already
  // executed before its mask. Ordinary entry-offset queries also follow joins.
  const std::set<const MedOp *> *AllowedDefinitions = nullptr;
  // All definitions are inserted before queries begin, so their addresses stay
  // stable during traversal. Only read-only proofs follow joins. Their
  // completed traversals retain the exact occurrence, work and depth proved.
  llvm::DenseMap<Key, ValueDefinition> Definitions;
  llvm::DenseMap<Key, CompletedProof> Completed;
  size_t ContextUses = 0;
  unsigned Deepest = 0;
  std::map<Key, int64_t> Assumed;
  bool CycleHit = false;
  static constexpr size_t kMaxProofSteps =
      ::neverd::limits::ir::kMedStackAlignmentMaxProofSteps;
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

std::vector<std::optional<int64_t>> entryStackOffsetsFromProvenDefinitions(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
    llvm::ArrayRef<ProvenStackOffset> Seeds, Arch Architecture,
    BinaryFormat Format, StackEntryKind Entry) {
  std::vector<std::optional<int64_t>> Offsets(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Offsets;
  StackOffsetProof Proof(Func, Architecture, Format, Entry,
                         /*FollowJoins=*/true, Seeds);
  for (size_t I = 0; I < Values.size(); ++I)
    Offsets[I] = Proof.offset(Values[I]);
  return Offsets;
}

std::vector<std::optional<int64_t>> stackOffsetsRelativeToDefinition(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values, const MedOp &Root,
    Arch Architecture, BinaryFormat Format) {
  std::vector<std::optional<int64_t>> Offsets(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Offsets;
  const ProvenStackOffset Seed{&Root, 0};
  StackOffsetProof Proof(Func, Architecture, Format, StackEntryKind::Call,
                         /*FollowJoins=*/true, {Seed},
                         /*OnlySeededRoots=*/true);
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
