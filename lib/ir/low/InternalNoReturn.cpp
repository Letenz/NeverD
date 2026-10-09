//===- InternalNoReturn.cpp - No-return proofs for internal callees -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The LowIR counterpart of the MedIR no-return proof (MedNoReturn.h), run on
/// a callee lifted on demand so that a single-function decompile can cut the
/// path after a call into an internal routine that never returns, such as a
/// wrapper around a bug check.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/InternalNoReturn.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/med/MedNoReturn.h"

#include <optional>
#include <vector>

namespace neverd {

bool lowFunctionNeverReturns(const LowFunc &Func, Arch TheArch) {
  std::map<int, const LowBlock *> BlocksById;
  const LowBlock *Entry = nullptr;
  for (const LowBlock &Block : Func.Blocks) {
    BlocksById.emplace(Block.Id, &Block);
    if (!Entry && Block.StartAddr == Func.Entry)
      Entry = &Block;
  }
  if (!Entry)
    return false;
  std::vector<const LowBlock *> Pending{Entry};
  std::set<int> Visited{Entry->Id};
  bool SawTerminator = false;
  while (!Pending.empty()) {
    const LowBlock &Block = *Pending.back();
    Pending.pop_back();
    // Each instruction in order: a return ends the proof, a call marked as
    // not returning or an architectural trap ends the path.
    bool PathEnds = false;
    for (const LowInstructionBoundary &Insn : Block.InstructionBoundaries) {
      if (hasLowInstructionControlFlag(Insn.ControlFlags,
                                       LowInstructionControlFlag::NoReturn) &&
          !hasLowInstructionControlFlag(
              Insn.ControlFlags, LowInstructionControlFlag::Conditional)) {
        PathEnds = true;
        break;
      }
      for (uint64_t K = Insn.FirstOp;
           K < Insn.FirstOp + Insn.OpCount && K < Block.Ops.size(); ++K) {
        if (Block.Ops[K].Opcode == NdOp::RETURN)
          return false;
        if (isArchitecturalNoReturn(Block.Ops[K]))
          PathEnds = true;
      }
      if (PathEnds)
        break;
    }
    // Without instruction boundaries no call can be told apart as final.
    if (!Block.hasInstructionBoundaries())
      for (const LowOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::RETURN)
          return false;
    if (PathEnds) {
      SawTerminator = true;
      continue;
    }
    std::vector<int> Next(Block.Succs.begin(), Block.Succs.end());
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      // A handler outside the lifted body may return normally.
      if (Edge.BlockId < 0)
        return false;
      Next.push_back(Edge.BlockId);
    }
    // A path that stops without a terminator (an unresolved jump, a decode
    // failure) proves nothing.
    if (Next.empty())
      return false;
    for (int Id : Next) {
      auto It = BlocksById.find(Id);
      if (It == BlocksById.end())
        return false;
      if (Visited.insert(Id).second)
        Pending.push_back(It->second);
    }
  }
  return SawTerminator;
}

InternalNoReturnIndex::InternalNoReturnIndex(
    const BinaryImage &Img, const std::set<va_t> *KnownFuncEntries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const detail::AbsoluteRelocationRootIndex *Roots,
    const ExecutableCodeOwnerIndex *CodeOwners)
    : Img(Img), KnownFuncEntries(KnownFuncEntries),
      NoReturnTargets(NoReturnTargets), Roots(Roots), CodeOwners(CodeOwners) {}

bool InternalNoReturnIndex::neverReturns(va_t Target, unsigned Depth) const {
  if (Depth >= limits::kMaxNoReturnProofDepth ||
      !Img.hasExecutableCodeOwnerAt(Target))
    return false;
  const auto Key = std::make_pair(Target, Depth);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Proofs.find(Key); It != Proofs.end())
      return It->second;
  }
  // Two builds may prove the same callee at once; both reach the same
  // answer, so the lock is not held while lifting.
  Decoder Dec;
  bool Proved = false;
  if (Dec.init(Img)) {
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(KnownFuncEntries);
    Builder.setNoReturnTargetIndex(NoReturnTargets);
    Builder.setAbsoluteRelocationRootIndex(Roots);
    Builder.setExecutableCodeOwnerIndex(CodeOwners);
    Builder.setNoReturnCalleeProver(this, Depth + 1);
    const LowFunc Callee =
        Builder.build(Img, Dec, Target, Img.getFunctionNameAt(Target));
    Proved = lowFunctionNeverReturns(Callee, Img.Arch);
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  return Proofs.emplace(Key, Proved).first->second;
}

bool InternalNoReturnIndex::mayNeverReturn(va_t Target) const {
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Screens.find(Target); It != Screens.end())
      return It->second;
  }
  bool NothingReturns = false;
  va_t End = Img.getFunctionMetadataEnd(Target, CodeOwners);
  if (const ExceptionFunction *Unwind =
          Img.ExceptionMetadata.findFunctionEntry(Target);
      Unwind && Unwind->Kind == RuntimeFunctionKind::Primary &&
      Unwind->CodeRange.isValid())
    End = Unwind->CodeRange.End;
  const Segment *Seg = Img.getSegmentFor(Target);
  Decoder Dec;
  if (End != InvalidVA && End > Target &&
      End - Target <= limits::kMaxNoReturnScreenBytes && Seg &&
      Target >= Seg->VA && End - Seg->VA <= Seg->Data.size() && Dec.init(Img)) {
    NothingReturns = true;
    for (va_t At = Target; NothingReturns && At < End;) {
      DecodedInsn Insn;
      const int Size =
          Dec.selectMode(Img, At)
              ? Dec.decodeOne(Seg->Data.data() + (At - Seg->VA),
                              static_cast<size_t>(End - At), At, Insn)
              : 0;
      const std::optional<bool> Returns =
          Size > 0 ? Dec.returnsToCaller(Insn) : std::nullopt;
      NothingReturns = Returns && !*Returns;
      At += static_cast<va_t>(Size);
    }
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  return Screens.emplace(Target, NothingReturns).first->second;
}

} // namespace neverd
