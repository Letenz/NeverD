//===- CxxObjectActions.cpp - Destructor calls of FH4 object actions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/CxxObjectActions.h"

#include "../pass/HighFrameAddress.h"

#include "neverd/Limits.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {
namespace {

/// What a function's own code shows of its objects: the frame addresses it
/// computes, by their offset from the entry stack pointer, that pointer as
/// its code spells it, and the names its direct calls give their callees.
struct ObjectEvidence {
  std::optional<MedVar> EntrySP;
  std::set<int64_t> Offsets;
  std::map<va_t, std::string> CalleeNames;
};

ObjectEvidence objectEvidenceOf(const HighFunc &Func, Arch TargetArch) {
  ObjectEvidence Result;
  size_t Budget = limits::kMaxCxxObjectAddressWork;
  // A variable whose every value is one frame address spells that address,
  // as `v0 = frame_base - 88; ctor(v0 + 96)` shows an object at entry SP + 8.
  // Attached funclets share the parent's numbering, so locals are told apart
  // as the source names them.
  std::map<HighSourceLocalIdentity, std::optional<int64_t>> Bases;
  std::map<HighSourceLocalIdentity, ExprPtr> Aliases;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Assign || !S.Dst ||
        (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi))
      return;
    const HighSourceLocalIdentity Key = highSourceLocalIdentity(S.Dst->Var);
    const std::optional<int64_t> At =
        S.Dst->Kind == ExprKind::Var
            ? high_detail::frameAddressOffset(S.Val, Func, TargetArch, Budget)
            : std::nullopt;
    const auto [It, First] = Bases.try_emplace(Key, At);
    if (First && At)
      Aliases[Key] = S.Val;
    else if (!First && (!At || It->second != At)) {
      It->second.reset();
      Aliases.erase(Key);
    }
  });
  const auto Alias = [&](const MedVar &V) -> ExprPtr {
    const auto It = Aliases.find(highSourceLocalIdentity(V));
    return It == Aliases.end() ? nullptr : It->second;
  };
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E || !Budget)
      return;
    if (E->Kind == ExprKind::Var &&
        isSyntheticEntryStackPointer(E->Var, Func, TargetArch)) {
      if (!Result.EntrySP)
        Result.EntrySP = E->Var;
      return;
    }
    if (E->Kind == ExprKind::Call && E->CallAddr && !E->CallTarget.empty() &&
        !E->IndirectTarget)
      Result.CalleeNames.emplace(E->CallAddr, E->CallTarget);
    if (const std::optional<int64_t> Offset = high_detail::frameAddressOffset(
            E, Func, TargetArch, Budget, 0, Alias))
      Result.Offsets.insert(*Offset);
    E->forEachChildExpr(Visit);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) { forEachExpr(S, Visit); });
  return Result;
}

/// The destructor calls of \p Try's object actions, named as the function's
/// own calls name each destructor.  An object whose address the function
/// never computes keeps its action as a note: the call would name an object
/// the source does not show.
void addCxxObjectActionCalls(HighStmt &Try, const HighFunc &Func,
                             const ObjectEvidence &Evidence) {
  for (size_t C = 0; C < Try.EHClauses.size() && C < Try.EHClauseBodies.size();
       ++C) {
    const HighEHClause &Clause = Try.EHClauses[C];
    const bool ByObject = Clause.UnwindActionKind ==
                          CxxUnwindAction::ActionKind::DestructorWithObject;
    const bool ByPointer =
        Clause.UnwindActionKind ==
        CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
    if (Clause.Kind != HighEHClauseKind::CxxCleanup ||
        (!ByObject && !ByPointer) || !Clause.FilterOrActionVA ||
        !Try.EHClauseBodies[C].empty())
      continue;
    const int64_t Disp =
        static_cast<int64_t>(Clause.UnwindObjectOffset) - Func.FrameSize;
    if (!Evidence.Offsets.count(Disp))
      continue;
    const TypeRef Word = NdType::makeInt(8, /*S=*/false);
    ExprPtr Object = HighExpr::makeBinop(
        Disp < 0 ? NdOp::INT_SUB : NdOp::INT_ADD,
        HighExpr::makeVar(*Evidence.EntrySP, Word),
        HighExpr::makeConst(Disp < 0 ? uint64_t{0} - static_cast<uint64_t>(Disp)
                                     : static_cast<uint64_t>(Disp),
                            8, ConstantAddressProvenance::Scalar));
    // `this` is a pointer: the argument spells it as one.
    Object->Type = ByPointer ? Word : NdType::makePtr();
    ExprPtr This =
        ByPointer ? HighExpr::makeLoad(Object, NdType::makePtr()) : Object;
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    const auto Named = Evidence.CalleeNames.find(Clause.FilterOrActionVA);
    Call.CallExpr = HighExpr::makeCall(
        Named == Evidence.CalleeNames.end() ? std::string() : Named->second,
        Clause.FilterOrActionVA, {std::move(This)});
    Call.CallExpr->Type = NdType::makeVoid();
    Try.EHClauseBodies[C].push_back(std::move(Call));
  }
}

} // namespace

void attachCxxObjectActionCalls(HighFunc &Func, Arch TargetArch) {
  if (TargetArch != Arch::X64 || Func.FrameSize <= 0 ||
      !Func.ExceptionMetadata || Func.ExceptionMetadata->Registration ||
      !Func.ExceptionMetadata->Cxx)
    return;
  std::vector<HighStmt *> Tries;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (S.Kind == StmtKind::CxxTry && S.EHIsReducible)
      Tries.push_back(&S);
  });
  if (Tries.empty())
    return;
  // Each call fills only an empty cleanup body, so no try moves.
  const ObjectEvidence Evidence = objectEvidenceOf(Func, TargetArch);
  if (!Evidence.EntrySP)
    return;
  for (HighStmt *Try : Tries)
    addCxxObjectActionCalls(*Try, Func, Evidence);
}

} // namespace neverd
