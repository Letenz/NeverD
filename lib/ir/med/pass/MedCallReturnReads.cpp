//===- MedCallReturnReads.cpp - Source call-stack observations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep omitted physical CALL writes from becoming stale source-frame reads.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallReturnReads.h"

#include "../../../safety/StackSlotFlow.h"
#include "../abi/MedABIPassDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {
struct Access {
  const MedOp *Op;
  int Block;
  size_t Position;
  int64_t Begin;
  int64_t End;

  bool contains(int64_t Byte) const { return Begin <= Byte && Byte < End; }
};

bool charge(size_t &Remaining, size_t Count) {
  if (Count > Remaining) {
    Remaining = 0;
    return false;
  }
  Remaining -= Count;
  return true;
}
} // namespace

bool validateSourceCallReturnReads(const MedFunc &Func,
                                   const BinaryImage &Image,
                                   std::string &Diagnostic, size_t WorkLimit) {
  Diagnostic.clear();
  // Link-register targets have no implicit stack write. The current source
  // call encoding authority authenticates x64 near CALL/JMP occurrences.
  if (Image.Arch != Arch::X64 || Func.Blocks.empty())
    return true;
  auto Refuse = [&](const std::string &Reason) {
    Diagnostic = "source frame in " + Func.Name + ": " + Reason;
    return false;
  };
  auto Exhausted = [&] {
    return Refuse("CALL return-slot observation analysis exceeded its work "
                  "budget; refusing an unproved frame projection");
  };
  size_t Remaining = WorkLimit;
  std::vector<const MedOp *> Calls;
  std::vector<const MedOp *> Memory;
  std::map<const MedOp *, std::pair<int, size_t>> Positions;
  for (const MedBlock &Block : Func.Blocks) {
    if (!charge(Remaining, 1) || !charge(Remaining, Block.Ops.size()) ||
        !charge(Remaining, Block.Phis.size()) ||
        !charge(Remaining, Block.Preds.size()) ||
        !charge(Remaining, Block.Succs.size()))
      return Exhausted();
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Dead)
        continue;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Bias =
            sourceCallReturnAddressBias(&Image, Image.Arch, Op.Addr);
        // Synthetic tail calls must never acquire a physical push. Unknown
        // original encodings remain possible writes, not zero-byte writes.
        if (!Bias || *Bias != 0)
          Calls.push_back(&Op);
      }
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          ((Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) ||
           (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2))) {
        Memory.push_back(&Op);
        Positions.emplace(&Op, std::pair{Block.Id, I});
      }
    }
  }
  if (Calls.empty() || Memory.empty())
    return true;
  const auto StackPointers =
      sourceCallStackPointers(Func, Image.Arch, Remaining);
  if (!Remaining)
    return Exhausted();
  const auto Width = getTargetRegInfo(Image.Arch).PointerSize;
  std::vector<MedVar> Values;
  for (const auto *Op : Memory)
    Values.push_back(Op->Inputs[0]);
  for (const auto *Op : Calls) {
    const auto It = StackPointers.find(Op);
    Values.push_back(It == StackPointers.end()
                         ? MedVar::makeUnspecified(Width, Image.Arch)
                         : It->second);
  }
  const auto Offsets = entryStackOffsets(Func, Values, Image.Arch,
                                         Image.abiFormat(), Func.EntryKind);
  std::vector<Access> Loads;
  std::map<const MedOp *, Access> Stores;
  for (size_t I = 0; I < Memory.size(); ++I) {
    const auto *Op = Memory[I];
    const auto Size =
        Op->Opcode == NdOp::LOAD ? Op->Output.Size : Op->Inputs[1].Size;
    if (!Offsets[I] || !Size ||
        *Offsets[I] > std::numeric_limits<int64_t>::max() - Size)
      continue;
    const auto [Block, Position] = Positions.at(Op);
    Access A{Op, Block, Position, *Offsets[I], *Offsets[I] + Size};
    if (Op->Opcode == NdOp::LOAD)
      Loads.push_back(A);
    else if (Op->MemoryOrdering == NdMemoryOrdering::None &&
             Op->MemoryAddressSpace == NdMemoryAddressSpace::Default)
      Stores.emplace(Op, A);
  }
  std::map<const MedOp *, std::optional<int64_t>> CallEnds;
  for (size_t I = 0; I < Calls.size(); ++I)
    CallEnds.emplace(Calls[I], Offsets[Memory.size() + I]);
  std::vector<std::pair<int64_t, int64_t>> Ranges;
  bool UnknownLocation = false;
  for (const auto &[Call, End] : CallEnds) {
    if (!End || *End < std::numeric_limits<int64_t>::min() + Width) {
      UnknownLocation = true;
      continue;
    }
    Ranges.emplace_back(*End - Width, *End);
  }
  llvm::sort(Ranges);
  std::vector<std::pair<int64_t, int64_t>> Merged;
  for (const auto &Range : Ranges) {
    if (Merged.empty() || Merged.back().second < Range.first)
      Merged.push_back(Range);
    else
      Merged.back().second = std::max(Merged.back().second, Range.second);
  }
  auto OverlapsCall = [&](const Access &Load) {
    if (UnknownLocation)
      return true;
    const auto It = std::lower_bound(
        Merged.begin(), Merged.end(), Load.Begin,
        [](const auto &Range, int64_t Begin) { return Range.second <= Begin; });
    return It != Merged.end() && It->first < Load.End;
  };
  if (llvm::none_of(Loads, OverlapsCall))
    return true;
  const MedBlockIdIndex Blocks(Func);
  const safety::detail::ExactPhiGraph Graph(Func);
  for (const MedBlock &Block : Func.Blocks)
    if (!Graph.hasCompleteNormalPredecessors(Block) ||
        !Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return Refuse("CALL return-slot observation requires a complete normal "
                    "control-flow graph");
  // Walk only candidate bytes. A store on one predecessor must not hide an
  // implicit CALL write reaching the load through another predecessor or loop.
  for (const Access &Load : Loads) {
    if (!OverlapsCall(Load))
      continue;
    for (int64_t Byte = Load.Begin; Byte < Load.End; ++Byte) {
      using State = std::pair<int, size_t>;
      std::vector<State> Pending{{Load.Block, Load.Position}};
      std::set<State> Seen;
      while (!Pending.empty()) {
        if (!charge(Remaining, 1))
          return Exhausted();
        const auto Current = Pending.back();
        Pending.pop_back();
        if (!Seen.insert(Current).second)
          continue;
        const auto *Block = Blocks.first(Current.first);
        if (!Block)
          return Refuse("CALL return-slot observation has a missing block");
        bool Written = false;
        for (size_t I = Current.second; I > 0; --I) {
          if (!charge(Remaining, 1))
            return Exhausted();
          const auto &Op = Block->Ops[I - 1];
          if (const auto It = Stores.find(&Op);
              It != Stores.end() && It->second.contains(Byte)) {
            Written = true;
            break;
          }
          const auto Call = CallEnds.find(&Op);
          if (Call == CallEnds.end())
            continue;
          if (!Call->second ||
              *Call->second < std::numeric_limits<int64_t>::min() + Width ||
              (Byte < *Call->second && Byte >= *Call->second - Width))
            return Refuse("load at 0x" + llvm::utohexstr(Load.Op->Addr) +
                          " observes a physical CALL return slot from 0x" +
                          llvm::utohexstr(Op.Addr) +
                          "; the ordinary C frame does not represent that "
                          "write; refusing a stale frame value");
        }
        if (Written)
          continue;
        if (!charge(Remaining, Block->Preds.size()))
          return Exhausted();
        for (int Id : Block->Preds) {
          const auto *Pred = Blocks.first(Id);
          if (!Pred)
            return Refuse(
                "CALL return-slot observation has a missing predecessor");
          Pending.emplace_back(Id, Pred->Ops.size());
        }
      }
    }
  }
  return true;
}
} // namespace neverd
