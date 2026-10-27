//===- HighVaStartForwarder.cpp - A va_list built only to pass it on ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A System V x86-64 variadic function saves its argument registers to a
/// register save area and builds a va_list over it and its incoming stack
/// arguments.  Where it does nothing with them but pass the va_list's address
/// to parameters that take a va_list (`error(fmt, ...)` calling
/// `verror(fmt, va)`; MedFunc::VaListCalls), C says the same with `va_start`
/// on a `va_list` of its own: the C writer prints the function as
/// `T f(named, ...)`, and each call of it with the arguments its caller
/// passes.  Anything else the body does with the save area, the va_list or
/// the overflow homes keeps the function as it is.
///
//===----------------------------------------------------------------------===//

#include "HighFrameAddress.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace neverd {
namespace {

constexpr int64_t kVaListBytes = HighFunc::VaStartForwarder::VaListBytes;
constexpr int64_t kSaveAreaBytes = HighFunc::VaStartForwarder::SaveAreaBytes;

/// \p E without the conversions a spill puts around the parameter it saves:
/// a vector register's double is stored as its bits, widened to the slot,
/// and a parameter narrower than its register reads as its low bytes.
const HighExpr *spilledValue(const HighExpr *E) {
  for (unsigned Depth = 0;
       E &&
       Depth <
           ::neverd::limits::ir::kHighVaStartForwarderSpilledValueDepthLimit;
       ++Depth) {
    if ((E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast ||
         (E->Kind == ExprKind::UnaryOp && E->Op == NdOp::INT_ZEXT)) &&
        E->Operands.size() == 1) {
      E = E->Operands[0].get();
      continue;
    }
    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
        E->Operands.size() == 2 && E->Operands[1] &&
        E->Operands[1]->Kind == ExprKind::Const &&
        E->Operands[1]->ConstVal == 0) {
      E = E->Operands[0].get();
      continue;
    }
    return E;
  }
  return nullptr;
}

} // namespace

void recognizeVaStartForwarder(HighFunc &Func, Arch Architecture) {
  Func.VaStart.reset();
  if (Architecture != Arch::X64 || !Func.IsVariadic ||
      !Func.VaListDisplacement || !Func.VaRegisterSaveArea ||
      Func.VaNamedIntegerRegisters < 1)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  const unsigned Named = static_cast<unsigned>(Func.VaNamedIntegerRegisters);
  // The named parameters are the first integer argument registers, in order.
  if (Named > TRI.IntParamRegs.size() || Func.Params.size() < Named)
    return;
  for (size_t I = 0; I < Named; ++I)
    if (Func.Params[I].RegOff != TRI.IntParamRegs[I])
      return;
  const int64_t VaList = *Func.VaListDisplacement;
  const int64_t SaveArea = *Func.VaRegisterSaveArea;
  const auto InVaList = [&](int64_t At) {
    return At >= VaList && At < VaList + kVaListBytes;
  };
  const auto InSaveArea = [&](int64_t At) {
    return At >= SaveArea && At < SaveArea + kSaveAreaBytes;
  };
  // The return address and the stack arguments past it, which hold the
  // overflow a va_arg walk would read.
  const auto Incoming = [](int64_t At) { return At >= 0; };
  const auto Reserved = [&](int64_t At) {
    return InVaList(At) || InSaveArea(At) || Incoming(At);
  };
  // A parameter reads as its slot in Func.Params.
  const auto ParamPosition = [&](const HighExpr *E) -> std::optional<size_t> {
    if (!E || (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi) ||
        E->Var.Kind != MedVar::Param || E->Var.Id < 0)
      return std::nullopt;
    return static_cast<size_t>(E->Var.Id);
  };

  // A local assigned once a frame address holds it at every use.
  std::map<HighSourceLocalIdentity, unsigned> Definitions;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi))
      ++Definitions[highSourceLocalIdentity(S.Dst->Var)];
  });
  // Every local whose value derives from the entry stack pointer.  Each must
  // hold one address, or an access through it could reach the areas this
  // proof does not see.
  const auto &EntrySp = [&](const HighExpr &E) {
    return (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
           isSyntheticEntryStackPointer(E.Var, Func, Architecture);
  };
  std::set<HighSourceLocalIdentity> FrameLocals;
  // Whether \p E's value may be a frame address: one computed from the entry
  // stack pointer, not one read from memory or returned by a call.
  std::function<bool(const HighExpr &, unsigned)> FrameValue = [&](const HighExpr
                                                                       &E,
                                                                   unsigned
                                                                       Depth) {
    if (Depth >
        ::neverd::limits::ir::
            kHighVaStartForwarderRecognizeVaStartForwarderFrameValueDepthLimit)
      return true;
    if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi)
      return EntrySp(E) || FrameLocals.count(highSourceLocalIdentity(E.Var));
    if (E.Kind != ExprKind::BinOp && E.Kind != ExprKind::UnaryOp &&
        E.Kind != ExprKind::Cast && E.Kind != ExprKind::BitCast)
      return false;
    for (const ExprPtr &Operand : E.Operands)
      if (Operand && FrameValue(*Operand, Depth + 1))
        return true;
    return false;
  };
  for (bool Grew = true; Grew;) {
    Grew = false;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
          (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
          FrameValue(*S.Val, 0) &&
          FrameLocals.insert(highSourceLocalIdentity(S.Dst->Var)).second)
        Grew = true;
    });
  }
  for (const HighSourceLocalIdentity &Local : FrameLocals)
    if (Definitions[Local] != 1)
      return;
  std::map<HighSourceLocalIdentity, ExprPtr> Aliases;
  size_t Budget = ::neverd::limits::ir::kHighVaStartForwarderAddressBudget;
  const auto Alias = [&](const MedVar &V) -> ExprPtr {
    const auto It = Aliases.find(highSourceLocalIdentity(V));
    return It == Aliases.end() ? nullptr : It->second;
  };
  const auto Offset = [&](const ExprPtr &E) {
    return high_detail::frameAddressOffset(E, Func, Architecture, Budget, 0,
                                           Alias);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        S.Dst->Var.Kind != MedVar::Stack &&
        S.Dst->Var.Size == TRI.PointerSize &&
        Definitions[highSourceLocalIdentity(S.Dst->Var)] == 1 && Offset(S.Val))
      Aliases.emplace(highSourceLocalIdentity(S.Dst->Var), S.Val);
  });

  bool Valid = true;
  std::set<std::pair<va_t, unsigned>> Passes, Junk;
  // An expression outside va_start's own stores: it may pass the va_list's
  // address to a call, and nothing else of the three areas or of the saved
  // parameters.
  std::function<void(const ExprPtr &)> Check = [&](const ExprPtr &E) {
    if (!E || !Valid)
      return;
    if (const auto Position = ParamPosition(E.get())) {
      if (*Position >= Named)
        Valid = false;
      return;
    }
    if (E->Kind == ExprKind::Load) {
      if (E->Operands.size() == 1 && E->Operands[0]) {
        const auto At = Offset(E->Operands[0]);
        if (At ? Reserved(*At) : FrameValue(*E->Operands[0], 0)) {
          Valid = false;
          return;
        }
      }
    } else if (E->Kind == ExprKind::Call) {
      for (size_t I = 0; I < E->Operands.size(); ++I) {
        const auto At = Offset(E->Operands[I]);
        if (!At || !Reserved(*At)) {
          Check(E->Operands[I]);
          continue;
        }
        // The C writer finds the operand by its call's address.
        if (!E->CallSiteAddr) {
          Valid = false;
          return;
        }
        // C's va_list goes only to a parameter that takes one.  Another
        // takes the frame's bytes, which C then has no object for: a buffer
        // the compiler placed where the list was once va_end was past.
        const auto Taken = Func.VaListCalls.find(E->CallSiteAddr);
        const bool Marked = *At == VaList && Taken != Func.VaListCalls.end();
        if (Marked && !Taken->second) {
          Valid = false;
          return;
        }
        (Marked ? Passes : Junk)
            .emplace(E->CallSiteAddr, static_cast<unsigned>(I));
      }
      Check(E->IndirectTarget);
      return;
    } else if (const auto At = Offset(E)) {
      if (Reserved(*At))
        Valid = false;
      return;
    } else if (FrameValue(*E, 0)) {
      // A frame address computed some other way may reach the areas.
      Valid = false;
      return;
    }
    for (const ExprPtr &Operand : E->Operands)
      Check(Operand);
    Check(E->IndirectTarget);
  };
  // Whether \p S is one of va_start's own stores: a field of the va_list, a
  // parameter saved to the register save area, or an overflow parameter
  // homed above the return address, or an alignment check of a save slot.
  const auto VaStartStore = [&](const HighStmt &S) {
    ExprPtr Address, Value;
    if (S.Kind == StmtKind::Store) {
      Address = S.StoreAddr;
      Value = S.StoreVal;
    } else if (S.Kind == StmtKind::Assign && S.Dst &&
               S.Dst->Kind == ExprKind::Load && S.Dst->Operands.size() == 1) {
      Address = S.Dst->Operands[0];
      Value = S.Val;
    } else if (S.Kind == StmtKind::Call && S.CallExpr &&
               S.CallExpr->IntrinsicId == Intrinsic::RequireAligned &&
               !S.CallExpr->Operands.empty()) {
      const auto At = Offset(S.CallExpr->Operands[0]);
      return At && InSaveArea(*At);
    }
    if (!Address || !Value)
      return false;
    const auto At = Offset(Address);
    if (!At)
      return false;
    if (InVaList(*At))
      return Value->Kind == ExprKind::Const || Offset(Value).has_value();
    if (InSaveArea(*At) || Incoming(*At)) {
      const auto Position = ParamPosition(spilledValue(Value.get()));
      return Position && *Position >= Named;
    }
    return false;
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (!Valid)
      return;
    // A frame alias's own assignment names its address; its uses count.
    if (S.Kind == StmtKind::Assign && S.Dst &&
        (S.Dst->Kind == ExprKind::Var || S.Dst->Kind == ExprKind::Phi) &&
        Aliases.count(highSourceLocalIdentity(S.Dst->Var)))
      return;
    if (VaStartStore(S))
      return;
    // A frame address stored to memory could come back through a load the
    // proof cannot follow.
    const ExprPtr &Stored =
        S.Kind == StmtKind::Store ? S.StoreVal
        : S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Load
            ? S.Val
            : nullptr;
    if (Stored && FrameValue(*Stored, 0)) {
      Valid = false;
      return;
    }
    forEachExpr(S, Check);
  });
  if (!Valid || !Budget)
    return;
  Func.VaStart = HighFunc::VaStartForwarder{VaList, SaveArea, Named,
                                            std::move(Passes), std::move(Junk)};
}

} // namespace neverd
