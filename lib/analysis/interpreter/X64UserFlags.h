//===- X64UserFlags.h - Shared packed flags semantics -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_X64_USER_FLAGS_H
#define NEVERD_ANALYSIS_X64_USER_FLAGS_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>
#include <utility>

namespace neverd::analysis::detail {

/// UserX64NoFaultV1 only. Intel SDM 253667-093, Table 4-20 and 64-bit
/// POPFQ pseudocode (4-408/409), PUSHFQ (4-528/529). RF is cleared; TF/AC
/// are architecturally writable but deliberately excluded by this profile.
struct X64UserFlags {
  static constexpr uint32_t SemanticsVersion = 1;
  static constexpr uint64_t SplitMask = 0xcd5;
  static constexpr uint64_t SystemWriteMask =
      (uint64_t{1} << 14) | (uint64_t{1} << 21);
  static constexpr uint64_t RejectedWriteMask =
      (uint64_t{1} << 8) | (uint64_t{1} << 18);
  static constexpr uint64_t ResumeMask = uint64_t{1} << 16;
  static constexpr uint64_t SnapshotClearMask =
      ResumeMask | (uint64_t{1} << 17);
  static constexpr uint64_t EntryMask =
      SplitMask | SystemWriteMask | (uint64_t{1} << 9) | 2;
  static constexpr std::pair<uint64_t, unsigned> Flags[] = {
      {x86reg::CF, 0}, {x86reg::PF, 2},  {x86reg::AF, 4}, {x86reg::ZF, 6},
      {x86reg::SF, 7}, {x86reg::DF, 10}, {x86reg::OF, 11}};

  static bool validEntry(uint64_t Image) {
    return (Image & ~EntryMask) == 0 && (Image & 2) != 0;
  }
};

struct X64UserFlagsTransition {
  std::vector<LowOp> Ops;
  /// A canonical byte Boolean. Consumers must either record sticky failure
  /// (source ABI) or prove both executions cannot set it (native proof).
  std::optional<NdVar> Rejected;
};

/// A pure lowering shared by source generation and native proof. It owns only
/// the system portion; original lifter ops still pack/scatter scalar flags and
/// perform the actual stack accesses. Temporary allocation must be disjoint
/// from all supplied values. No host flags or path assumption enters this code.
inline std::optional<X64UserFlagsTransition>
lowerX64UserFlags(const LowOp &IntrinsicOp, NdVar System,
                  llvm::function_ref<NdVar(uint16_t)> Temporary) {
  if (IntrinsicOp.Opcode != NdOp::INTRINSIC ||
      IntrinsicOp.MemoryOrdering != NdMemoryOrdering::None ||
      IntrinsicOp.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !IntrinsicOp.NumInputs || !IntrinsicOp.Inputs[0].isConst() ||
      IntrinsicOp.Inputs[0].Size != 2)
    return std::nullopt;
  const auto Kind = IntrinsicOp.Inputs[0].Offset;
  const bool Push = Kind == static_cast<uint64_t>(Intrinsic::Pushf);
  if (Push ? (IntrinsicOp.NumInputs != 1 || !IntrinsicOp.Output.isTemp() ||
              IntrinsicOp.Output.Size != 8)
           : (Kind != static_cast<uint64_t>(Intrinsic::Popf) ||
              IntrinsicOp.NumInputs != 2 || IntrinsicOp.Output.Size ||
              IntrinsicOp.Inputs[1].Size != 8))
    return std::nullopt;
  X64UserFlagsTransition Result;
  const auto Emit = [&](NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Code;
    Op.Output = Output;
    Op.Addr = IntrinsicOp.Addr;
    Op.Seq = static_cast<int>(Result.Ops.size());
    for (NdVar Input : Inputs)
      Op.addInput(Input);
    Result.Ops.push_back(Op);
  };
  const auto Constant = [](uint64_t Value) { return NdVar::scalar(Value, 8); };
  if (Push) {
    Emit(NdOp::INT_AND, IntrinsicOp.Output,
         {System, Constant(~X64UserFlags::SnapshotClearMask)});
  } else {
    const NdVar Image = IntrinsicOp.Inputs[1];
    const NdVar BadBits = Temporary(8), Bad = Temporary(1);
    Emit(NdOp::INT_AND, BadBits,
         {Image, Constant(X64UserFlags::RejectedWriteMask)});
    Emit(NdOp::INT_NOTEQUAL, Bad, {BadBits, Constant(0)});
    Result.Rejected = Bad;
    const NdVar Kept = Temporary(8), Changed = Temporary(8);
    Emit(NdOp::INT_AND, Kept,
         {System, Constant(~(X64UserFlags::SystemWriteMask |
                             X64UserFlags::ResumeMask))});
    Emit(NdOp::INT_AND, Changed,
         {Image, Constant(X64UserFlags::SystemWriteMask)});
    Emit(NdOp::INT_OR, System, {Kept, Changed});
  }
  return Result;
}

} // namespace neverd::analysis::detail

#endif
