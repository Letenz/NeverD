//===- MedVariadicX86.cpp - x86 variadic prologues ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The x86 variadic prologue recognizers: the System V x86-64 va_start
/// word with its register save area, and the i386 stack va_list home slot.
///
//===----------------------------------------------------------------------===//

#include "MedVariadicDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include "llvm/ADT/SmallVector.h"

namespace neverd {
namespace med_variadic_detail {

using med_calling_conv_detail::computeForwardValueClosure;
using med_calling_conv_detail::containsValue;

/// The va_start GP/FP-offset word with a parameter-register spill.
bool hasX64VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  bool Marked = false;
  // The va_start GP/FP-offset word identifies a variadic prologue; pairing it
  // with a parameter-register spill keeps a stray constant from qualifying.
  bool HasVaWord = false;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops) {
      if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && isX64VaStartWord(Op.Inputs[I].ConstVal))
          HasVaWord = true;
    }
  Marked = HasVaWord && countParamRegSpills(Func, TRI.IntParamRegs) >= 1;
  return Marked;
}

/// The i386 stack va_list, homed and then reloaded or walked.
bool hasI386VariadicPrologue(VariadicScan &S) {
  const MedFunc &Func = S.Func;
  const TargetRegInfo &TRI = S.TRI;
  const uint64_t SpOff = S.SpOff;
  auto getValueIndex = [&S]() -> const VariadicValueIndex & {
    return S.values();
  };
  bool Marked = false;
  // i386 cdecl passes every argument (named and variadic) on the stack, so a
  // variadic prologue has no register save area.  va_start instead points a
  // va_list at the first unnamed argument -- entry_sp + PointerSize (return
  // address) + named-argument bytes, an entry-SP-positive pointer at least
  // two slots above entry SP -- and spills it to a home slot in the callee
  // frame; va_arg reloads that slot and walks it.  The tell-tale, robust
  // against
  // `&secondParam` (stored as an outgoing arg but never reloaded) and against
  // a plain incoming-stack-parameter read (loaded but never stored as a
  // pointer), is BOTH: an entry-SP-positive pointer (delta >= 2*PointerSize)
  // stored to a frame slot S, AND a reload from that same slot S.
  const int64_t MinPtrDelta = 2 * TRI.PointerSize;
  std::set<int64_t> HomeSlots;
  llvm::SmallVector<MedVar, 2> DirectSeeds;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto VD = entrySpDelta(getValueIndex(), SpOff, Op.Inputs[1], 0))
          if (*VD >= MinPtrDelta && *VD <= limits::kVariadicOverflowBaseMax &&
              TRI.PointerSize > 0 && (*VD % TRI.PointerSize) == 0)
            if (auto AD =
                    entrySpDelta(getValueIndex(), SpOff, Op.Inputs[0], 0)) {
              HomeSlots.insert(*AD);
              DirectSeeds.push_back(Op.Inputs[1]);
            }
  bool Reloaded = false;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default)
        if (auto AD = entrySpDelta(getValueIndex(), SpOff, Op.Inputs[0], 0))
          if (HomeSlots.count(*AD))
            Reloaded = true;

  // At -O2 the va_list pointer can remain live in registers after its
  // mandatory home store, so va_arg walks it directly and never reloads the
  // home slot.  Follow that seed through PHIs, width-preserving views, and
  // constant pointer advances; a load through the resulting value proves the
  // stored entry-SP-positive pointer is an active va_arg walk.
  auto forwardsTransparent = [](const MedOp &Op, unsigned InputIdx) {
    if (InputIdx != 0)
      return false;
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Op.NumInputs >= 1;
    case NdOp::SUBBYTES:
      return Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
             Op.Inputs[1].ConstVal == 0;
    default:
      return false;
    }
  };
  auto forwardsConstantAdvance = [](const MedOp &Op, unsigned InputIdx) {
    if (Op.NumInputs < 2)
      return false;
    if (Op.Opcode == NdOp::INT_ADD)
      return (InputIdx == 0 && Op.Inputs[1].isConst()) ||
             (InputIdx == 1 && Op.Inputs[0].isConst());
    return Op.Opcode == NdOp::INT_SUB && InputIdx == 0 &&
           Op.Inputs[1].isConst();
  };

  auto DirectWalkPtrs = computeForwardValueClosure(
      Func, DirectSeeds, [&](const MedOp &Op, unsigned InputIdx) {
        return forwardsTransparent(Op, InputIdx) ||
               forwardsConstantAdvance(Op, InputIdx);
      });
  llvm::SmallVector<MedVar, 4> AdvancedSeeds;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (forwardsConstantAdvance(Op, I) &&
            containsValue(DirectWalkPtrs, Op.Inputs[I])) {
          AdvancedSeeds.push_back(Op.Output);
          break;
        }
  auto AdvancedWalkPtrs =
      computeForwardValueClosure(Func, AdvancedSeeds, forwardsTransparent);

  bool UsedAsLoad = false;
  for (const auto &Blk : Func.Blocks)
    for (const auto &Op : Blk.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          containsValue(AdvancedWalkPtrs, Op.Inputs[0]))
        UsedAsLoad = true;
  Marked = !HomeSlots.empty() && (Reloaded || UsedAsLoad);
  return Marked;
}

} // namespace med_variadic_detail
} // namespace neverd
