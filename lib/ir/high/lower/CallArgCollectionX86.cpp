//===- CallArgCollectionX86.cpp - x86 / x86-64 call-argument ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 stack-argument recovery.  Both use the shared spilled-store
/// scan (slot width is TRI.PointerSize).  i386 cdecl/stdcall additionally
/// pushes each argument, so every STORE looks like slot 0; walk those pushes
/// backward (last push is arg0).
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <map>
#include <optional>

namespace neverd {
namespace call_args_detail {

void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args) {
  collectSpilledStackArgs(Scan, Found);
  const BinaryFormat Format =
      Scan.Image ? Scan.Image->Format : BinaryFormat::Unknown;
  const size_t RegisterPositions =
      Scan.TRI->integerArgumentLayout(Format).Registers.size();
  // A summarized callee reads no stack slot past the last one its body, or
  // a tail call it makes, reads: an outgoing-area store beyond that belongs
  // to another call or is a local.
  if (Scan.Convention && Scan.Convention->StackArgumentSummary &&
      Scan.CalleeRegisterArgs >= 0 && Scan.CalleeStackArgs >= 0) {
    const int FirstStackArg = static_cast<int>(RegisterPositions);
    for (size_t K = std::max(FirstStackArg, Scan.CalleeStackArgs);
         K < Found.size(); ++K)
      Found[K] = nullptr;
  }
  // A tail jump leaves its own incoming stack arguments in place, so a slot
  // the callee reads (a position below its CalleeStackArgs) that the body
  // did not store passes the parameter there.  Stack positions start past
  // the register ones, or past those used where they follow the used
  // registers (i386).
  const int FirstStackPosition =
      Scan.Convention && Scan.Convention->StackArgumentsFollowUsedRegisters
          ? Scan.FirstStackSlot
          : static_cast<int>(RegisterPositions);
  if (Scan.TailJump && Scan.OwnStackParam)
    for (int K = FirstStackPosition;
         K < Scan.CalleeStackArgs && K < static_cast<int>(Found.size()); ++K)
      if (!Found[K])
        Found[K] = Scan.OwnStackParam(K);
  // With positional slots a stack argument means every register argument is
  // passed, even the ones this block did not write (a pass-through of the
  // caller's own).
  if (Scan.Convention && Scan.Convention->PositionalArgumentSlots &&
      Scan.ReachingRegArg &&
      std::any_of(Found.begin() + std::min(RegisterPositions, Found.size()),
                  Found.end(), [](const ExprPtr &E) { return E != nullptr; }))
    for (int K = 0; K < static_cast<int>(RegisterPositions) &&
                    K < static_cast<int>(Found.size());
         ++K)
      if (!Found[K]) {
        // A slot the callee does not read still occupies its position.  A
        // direct callee whose summary gave the register arguments reads none
        // past them, so a stale or unknown value the caller left there
        // cannot be observed; a dispatcher's count only says which registers
        // the caller set, and this function's own parameter still passes
        // through as an untouched forwarder's does.
        const bool Unread = Scan.CalleeRegisterArgs >= 0 &&
                            K >= Scan.CalleeRegisterArgs &&
                            (*Scan.Ops)[Scan.CallIdx].Opcode == NdOp::CALL &&
                            !(Scan.IsOwnParameter && Scan.IsOwnParameter(K));
        if (!Unread)
          Found[K] = Scan.ReachingRegArg(K);
        if (!Found[K])
          Found[K] = HighExpr::makeConst(0, 8);
      }
  for (int K = 0; K < Scan.MaxArgs; ++K) {
    if (!Found[K])
      break;
    Args.push_back(Found[K]);
  }
  if (Scan.TheArch != Arch::X86 || Scan.TailJump)
    return;

  // Every i386 `push` stores at the current ESP, so the spilled-slot scan
  // sees StackOff 0 for each of them and keeps only the last push (arg0).
  // Walk STORE values backward (last push is arg0). Call-only IP-map splits
  // leave the earlier pushes in ExtraWindows.  Where the call's stack pointer
  // is known, each store instead takes the outgoing slot it stands at from
  // it (callStackOffset), and a later store to a slot replaces an earlier
  // one: a local written between the pushes, or a push popped since (the
  // get-PC `call $+5; pop ebx`), is no argument, and GCC's `mov [esp+N]`
  // stores need not come in slot order.
  std::vector<ExprPtr> Pushed;
  std::map<int64_t, ExprPtr> Slots;
  const int64_t SlotBytes = Scan.TRI->PointerSize;
  auto walkPushes = [&](const std::vector<MedOp> &Ops, int Before) {
    for (int J = Before; J >= 0; --J) {
      const MedOp &Prev = Ops[static_cast<size_t>(J)];
      if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
          Prev.Opcode == NdOp::INTRINSIC)
        break;
      if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
          Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      if (Scan.IsCalleeSave(Prev.Inputs[1]))
        continue;
      if (Scan.PlacesOutgoingStores) {
        const std::optional<int64_t> Offset =
            callStackOffset(Scan, Ops, J, Prev.Inputs[0]);
        if (Offset && *Offset >= 0 && *Offset % SlotBytes == 0)
          Slots.try_emplace(*Offset / SlotBytes, Scan.ToExpr(Prev.Inputs[1]));
        continue;
      }
      Pushed.push_back(Scan.ToExpr(Prev.Inputs[1]));
      if (static_cast<int>(Pushed.size()) == Scan.MaxArgs)
        return;
    }
  };
  if (Scan.Ops)
    walkPushes(*Scan.Ops, static_cast<int>(Scan.CallIdx) - 1);
  if (static_cast<int>(Pushed.size()) < Scan.MaxArgs)
    for (const auto &W : Scan.ExtraWindows)
      if (W.Ops) {
        walkPushes(*W.Ops, W.Before);
        if (static_cast<int>(Pushed.size()) == Scan.MaxArgs)
          break;
      }
  for (int K = 0; K < Scan.MaxArgs; ++K) {
    auto Slot = Slots.find(K);
    if (Slot == Slots.end())
      break;
    Pushed.push_back(Slot->second);
  }
  if (Pushed.size() <= Args.size())
    return;
  for (size_t K = 0; K < Pushed.size() && K < Found.size(); ++K)
    Found[K] = Pushed[K];
  Args = std::move(Pushed);
}

/// i386 cdecl and stdcall push their arguments, and a block boundary between
/// the pushes and a call alone in its block leaves them in its predecessor.
/// ECX and EDX there are not arguments.  GCC instead stores each argument at
/// [esp+N] below one adjustment, in several ops apiece.
extern const CallArgPolicy I386CallArgPolicy;
const CallArgPolicy I386CallArgPolicy = {
    .TheArch = Arch::X86,
    .ReadsPredecessorWindow = true,
    .PlacesOutgoingStores = true,
};

} // namespace call_args_detail
} // namespace neverd
