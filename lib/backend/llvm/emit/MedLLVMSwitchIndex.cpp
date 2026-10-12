//===- MedLLVMSwitchIndex.cpp - Switch index recovery ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Switch-index recovery for MedLLVMEmitter: tracing an INDIR_BR target
/// back to the scaled index of its table load, in-block and across the
/// frame spill of an -O0 computed-goto dispatch, plus the constant-index
/// table-load folding and the per-predecessor table-base check that keeps
/// a shared multi-site dispatch from merging two distinct tables.  The
/// recovered index is consumed by the switch lowering in
/// MedLLVMSwitch.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedDispatchSlots.h"
#include "neverd/ir/med/MedSwitchNorm.h"

#define DEBUG_TYPE "neverd-med-llvm-switch"
#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

MedDispatchContext MedLLVMEmitter::dispatchContext() const {
  MedDispatchContext Context;
  Context.Func = CurMedFunc;
  Context.Img = Img;
  Context.AddressBytes = Img && Img->getPointerSize() != 0
                             ? static_cast<uint16_t>(Img->getPointerSize())
                             : getTargetRegInfo(TargetArch).PointerSize;
  if (CurMedFunc)
    Context.DefOf = [this](const MedVar &V) { return lookupDef(V); };
  Context.SlotKeyOf = [this](const MedVar &V) { return addrSlotKey(V); };
  Context.ModelZero = [this](const MedVar &V) {
    return valueIsAuthenticatedModelZero(V);
  };
  return Context;
}

/// Trace an INDIR_BR target var back to the scaled index of its table load.
/// The index is the non-constant operand of the INT_MULT/INT_LEFT that scales
/// it, so this skips the table base and finds the genuine switch variable.
std::optional<MedVar>
MedLLVMEmitter::findSwitchIndex(const MedBlock &Blk,
                                const MedVar &Target) const {
  return findTableSwitchIndex(dispatchContext(), Blk, Target);
}

std::optional<std::pair<std::pair<int, int>, int64_t>>
MedLLVMEmitter::reloadSlotKeyOf(const MedBlock &Blk, const MedOp &BrOp) const {
  return dispatchReloadSlot(dispatchContext(), Blk, BrOp);
}

std::optional<MedVar> MedLLVMEmitter::tracePredSwitchIndex(
    const MedBlock &Pred,
    const std::pair<std::pair<int, int>, int64_t> &SlotKey) const {
  return predecessorSwitchIndex(dispatchContext(), Pred, SlotKey);
}

std::optional<MedVar> MedLLVMEmitter::constIndexFromTableLoad(
    const MedVar &Val,
    const std::function<const MedOp *(const MedVar &)> &defOf,
    const JumpTable &JT, bool RequireAuthenticatedLoad) const {
  return constTableLoadIndex(dispatchContext(), Val, defOf, JT,
                             RequireAuthenticatedLoad);
}

std::optional<MedVar> MedLLVMEmitter::tracePredConstDispatchIndex(
    const MedBlock &Pred,
    const std::pair<std::pair<int, int>, int64_t> &SlotKey,
    const JumpTable &JT) const {
  return predecessorConstDispatchIndex(dispatchContext(), Pred, SlotKey, JT);
}

std::optional<MedVar>
MedLLVMEmitter::traceConstBranchIndex(const MedOp &BrOp, const JumpTable &JT,
                                      bool RequireAuthenticatedLoad) const {
  if (!CurMedFunc || BrOp.NumInputs < 1 || BrOp.Inputs[0].isConst())
    return std::nullopt;
  // The table read may sit in a dominating block (a hoisted load), so use the
  // canonical function-wide definition index. Its identity includes Kind;
  // register/temp Id collisions must not select another value's LOAD.
  auto defOf = [&](const MedVar &V) -> const MedOp * { return lookupDef(V); };
  return constIndexFromTableLoad(BrOp.Inputs[0], defOf, JT,
                                 RequireAuthenticatedLoad);
}

std::optional<uint64_t> MedLLVMEmitter::predTableBaseVA(
    const MedBlock &Pred,
    const std::pair<std::pair<int, int>, int64_t> &SlotKey) const {
  return predecessorTableBase(dispatchContext(), Pred, SlotKey);
}

std::optional<MedVar>
MedLLVMEmitter::findSwitchIndexCrossBlock(const MedBlock &Blk,
                                          const MedOp &BrOp) const {
  if (!CurMedFunc)
    return std::nullopt;
  // Only a single-predecessor dispatch is unambiguous: the index is computed in
  // the one block that dominates the branch.  Multiple predecessors are routed
  // through synthesizeSharedDispatchIndex instead.
  if (Blk.Preds.size() != 1)
    return std::nullopt;
  auto SlotKey = reloadSlotKeyOf(Blk, BrOp);
  if (!SlotKey)
    return std::nullopt;
  const MedBlock *Pred = nullptr;
  for (auto &B : CurMedFunc->Blocks)
    if (B.Id == Blk.Preds[0]) {
      Pred = &B;
      break;
    }
  if (!Pred)
    return std::nullopt;
  // The index is defined in the dominating predecessor, so getVar in the
  // dispatch block reads it correctly via the alloca model.
  return tracePredSwitchIndex(*Pred, *SlotKey);
}

} // namespace neverd
