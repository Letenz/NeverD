//===- CallArgCollectionX86.cpp - x86 / x86-64 call-argument ABI ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 and x86-64 stack-argument recovery.  Both use the shared spilled-store
/// scan (slot width is TRI.PointerSize).  i386 cdecl/stdcall additionally
/// pushes each argument, so every STORE looks like slot 0: place each store
/// by its offset from the stack pointer the call is made with, or else walk
/// the pushes backward (last push is arg0).
///
//===----------------------------------------------------------------------===//

#include "CallArgCollectionDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>

namespace neverd {
namespace call_args_detail {

void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args) {
  collectSpilledStackArgs(Scan, Found);
  const BinaryFormat Format =
      Scan.Image ? Scan.Image->abiFormat() : BinaryFormat::Unknown;
  const size_t RegisterPositions =
      Scan.TRI->integerArgumentLayout(Format).Registers.size();
  // The stores before the call, latest first, until an earlier call: a
  // callee-saved register's entry value stored there is a prologue save,
  // and a value computed into one is an argument (clang -O0 keeps them in
  // EBX, ESI and EDI).  \p Take returns false to stop.
  auto stores = [&](auto &&Take) {
    auto walk = [&](const std::vector<MedOp> &Ops, int Before) {
      for (int J = Before; J >= 0; --J) {
        const MedOp &Prev = Ops[static_cast<size_t>(J)];
        if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
            Prev.Opcode == NdOp::INTRINSIC)
          return true;
        if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
            Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
          continue;
        if (Scan.IsCalleeSave(Prev.Inputs[1]) && Prev.Inputs[1].SSAVer == 0)
          continue;
        if (!Take(Prev))
          return false;
      }
      return true;
    };
    if (Scan.Ops && !walk(*Scan.Ops, static_cast<int>(Scan.CallIdx) - 1))
      return;
    for (const auto &W : Scan.ExtraWindows)
      if (W.Ops && !walk(*W.Ops, W.Before))
        return;
  };
  // Where no outgoing area is reserved, the stack arguments are pushed, or
  // stored at the stack pointer the call is made with, one slot each past
  // the register positions.  Each push is at offset 0 from the stack pointer
  // it is made with, so the spilled-store scan above sees only the last;
  // place every store by its offset from the call's stack pointer instead,
  // as far as the callee's summary says it reads.
  const bool ReservedArea =
      Scan.Convention && Scan.Convention->ReservedOutgoingArea;
  if (Scan.TheArch == Arch::X64 && !ReservedArea && Scan.CallStackEntryOffset &&
      Scan.EntryOffsetOf &&
      Scan.CalleeStackArgs > static_cast<int>(RegisterPositions)) {
    const int64_t SlotBytes = static_cast<int64_t>(Scan.TRI->PointerSize);
    const int64_t StackSlots =
        Scan.CalleeStackArgs - static_cast<int64_t>(RegisterPositions);
    stores([&](const MedOp &Store) {
      const std::optional<int64_t> Entry = Scan.EntryOffsetOf(Store.Inputs[0]);
      if (!Entry || *Entry < *Scan.CallStackEntryOffset)
        return true;
      const int64_t Offset = *Entry - *Scan.CallStackEntryOffset;
      if (Offset % SlotBytes != 0 || Offset / SlotBytes >= StackSlots)
        return true;
      const size_t K =
          RegisterPositions + static_cast<size_t>(Offset / SlotBytes);
      if (K < Found.size() && !Found[K])
        Found[K] = Scan.ToExpr(Store.Inputs[1]);
      return true;
    });
  }
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
  // leave the earlier pushes in ExtraWindows.  Every store counts toward
  // replacing the arguments below; the arguments leave out those that can
  // be none (IsNoArgumentStore), such as a local written between two pushes
  // or GCC's `mov [esp+N]` stores (MinGW's `mov [ebp-0xc], 0` beside them).
  //
  // With the stack pointer the call is made with known, a store's slot is
  // its offset from it instead: a push, a `mov [esp+N]` in any order, or
  // clang -O0's store through a copy of ESP.  A slot below it (a get-PC
  // push the code popped again) or past the callee's arguments holds none.
  //
  // Stores outnumbering the arguments found make a cdecl call, whose ECX
  // and EDX written before it are scratch (clang -O0 loads a value there
  // before storing it).
  size_t StoreCount = 0;
  stores([&](const MedOp &) {
    return ++StoreCount < static_cast<size_t>(Scan.MaxArgs);
  });
  if (StoreCount <= Args.size())
    return;
  if (Scan.CallStackEntryOffset && Scan.EntryOffsetOf) {
    const int64_t SlotBytes = static_cast<int64_t>(Scan.TRI->PointerSize);
    std::vector<ExprPtr> Slots(static_cast<size_t>(Scan.MaxArgs));
    stores([&](const MedOp &Store) {
      const std::optional<int64_t> Entry = Scan.EntryOffsetOf(Store.Inputs[0]);
      if (!Entry || *Entry < *Scan.CallStackEntryOffset)
        return true;
      const int64_t Offset = *Entry - *Scan.CallStackEntryOffset;
      if (Offset % SlotBytes != 0)
        return true;
      // A store wider than a slot, clang's `movsd [esp], xmm0` copying an
      // eight-byte structure, fills each slot it covers with its piece.
      const int64_t Width = Store.Inputs[1].Size;
      const int64_t Pieces =
          Width > SlotBytes && Width % SlotBytes == 0 ? Width / SlotBytes : 1;
      for (int64_t Piece = 0; Piece < Pieces; ++Piece) {
        const int64_t Index = Offset / SlotBytes + Piece;
        if (Index >= Scan.MaxArgs)
          break;
        ExprPtr &Slot = Slots[static_cast<size_t>(Index)];
        if (Slot)
          continue;
        ExprPtr Value = Scan.ToExpr(Store.Inputs[1]);
        if (Pieces > 1) {
          if (!Value->Type || Value->Type->Kind != NdTypeKind::Int)
            Value = HighExpr::makeBitCast(
                Value, NdType::makeInt(static_cast<uint16_t>(Width), false));
          Value = HighExpr::makeBinop(
              NdOp::SUBBYTES, Value,
              HighExpr::makeConst(static_cast<uint64_t>(Piece * SlotBytes), 4));
          Value->Type =
              NdType::makeInt(static_cast<uint16_t>(SlotBytes), false);
        }
        Slot = std::move(Value);
      }
      return true;
    });
    size_t Placed = 0;
    while (Placed < Slots.size() && Slots[Placed])
      ++Placed;
    if (Placed) {
      for (size_t K = 0; K < Placed && K < Found.size(); ++K)
        Found[K] = Slots[K];
      Args.assign(Slots.begin(), Slots.begin() + Placed);
      return;
    }
  }

  std::vector<ExprPtr> Pushed, Arguments;
  auto walkPushes = [&](const std::vector<MedOp> &Ops, int Before) {
    for (int J = Before; J >= 0; --J) {
      const MedOp &Prev = Ops[static_cast<size_t>(J)];
      if (Prev.Opcode == NdOp::CALL || Prev.Opcode == NdOp::INDIR_CALL ||
          Prev.Opcode == NdOp::INTRINSIC)
        break;
      if (Prev.Opcode != NdOp::STORE || Prev.NumInputs < 2 ||
          Prev.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        continue;
      if (Scan.IsCalleeSave(Prev.Inputs[1]) && Prev.Inputs[1].SSAVer == 0)
        continue;
      ExprPtr Value = Scan.ToExpr(Prev.Inputs[1]);
      if (!Scan.IsNoArgumentStore || !Scan.IsNoArgumentStore(Prev.Inputs[0]))
        Arguments.push_back(Value);
      Pushed.push_back(std::move(Value));
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
  if (Pushed.size() <= Args.size())
    return;
  for (size_t K = 0; K < Pushed.size() && K < Found.size(); ++K)
    Found[K] = K < Arguments.size() ? Arguments[K] : nullptr;
  Args = std::move(Arguments);
}

/// i386 cdecl and stdcall push their arguments, and a block boundary between
/// the pushes and a call alone in its block leaves them in its predecessor.
/// ECX and EDX there are not arguments.
extern const CallArgPolicy I386CallArgPolicy;
const CallArgPolicy I386CallArgPolicy = {
    .TheArch = Arch::X86,
    .ReadsPredecessorWindow = true,
};

} // namespace call_args_detail
} // namespace neverd
