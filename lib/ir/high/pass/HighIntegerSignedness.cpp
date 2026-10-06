//===- HighIntegerSignedness.cpp - Signedness of integer locals ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Chooses whether each register or temporary local is declared signed or
/// unsigned.  A machine value has no signedness; C operations do.  Wrapping
/// arithmetic, logical shifts, unsigned comparisons and zero extension are
/// plain C on an unsigned operand but need casts on a signed one, and the
/// signed counterparts the other way round.  Each local takes the
/// signedness most of its uses read, so the emitted C carries fewer casts;
/// the value bits, and therefore the semantics, do not change.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <map>
#include <optional>
#include <tuple>

namespace neverd {
namespace {

/// The writer names a register or temporary local by its rename tag, or by
/// its id and SSA version: one name, one declaration, one type.
using LocalKey = std::tuple<int, int64_t, int64_t>;

std::optional<LocalKey> localKey(const HighExpr &E) {
  if (E.Kind != ExprKind::Var && E.Kind != ExprKind::Phi)
    return std::nullopt;
  const MedVar &V = E.Var;
  if (V.Kind != MedVar::Reg && V.Kind != MedVar::Temp)
    return std::nullopt;
  if (V.RenameTag >= 0)
    return LocalKey{0, V.RenameTag, 0};
  return LocalKey{V.Kind == MedVar::Temp ? 1 : 2, V.Id, V.SSAVer};
}

enum class Reads { Neither, Signed, Unsigned };

/// What operand \p Index of \p Parent reads: the signedness its C spelling
/// needs to go without a cast.
Reads operandReads(const HighExpr &Parent, size_t Index) {
  if (Parent.Kind != ExprKind::BinOp && Parent.Kind != ExprKind::UnaryOp)
    return Reads::Neither;
  switch (Parent.Op) {
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_SDIV:
  case NdOp::INT_SREM:
  case NdOp::INT_SEXT:
    return Reads::Signed;
  case NdOp::INT_ASHR:
    return Index == 0 ? Reads::Signed : Reads::Neither;
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_DIV:
  case NdOp::INT_REM:
  case NdOp::INT_ZEXT:
  // Wrapping arithmetic is plain C only on unsigned operands.
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_NEG2:
    return Reads::Unsigned;
  case NdOp::INT_RIGHT:
  case NdOp::INT_LEFT:
    return Index == 0 ? Reads::Unsigned : Reads::Neither;
  default:
    return Reads::Neither;
  }
}

bool wrapsUnsigned(const HighExpr &E) {
  return (E.Kind == ExprKind::BinOp &&
          (E.Op == NdOp::INT_ADD || E.Op == NdOp::INT_SUB ||
           E.Op == NdOp::INT_MULT)) ||
         (E.Kind == ExprKind::UnaryOp && E.Op == NdOp::INT_NEG2);
}

bool isPlainInteger(const TypeRef &Type) {
  return !Type || (Type->Kind == NdTypeKind::Int && !Type->IsEnum);
}

template <typename F> void forEachStmt(std::vector<HighStmt> &Stmts, F &&Fn) {
  for (HighStmt &S : Stmts) {
    Fn(S);
    forEachStmt(S.Body, Fn);
    forEachStmt(S.ElseBody, Fn);
    for (SwitchCase &Case : S.Cases)
      forEachStmt(Case.Body, Fn);
    forEachStmt(S.DefaultBody, Fn);
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      forEachStmt(Clause, Fn);
  }
}

} // namespace

void chooseIntegerSignedness(HighFunc &Func) {
  struct Tally {
    unsigned Signed = 0;
    unsigned Unsigned = 0;
    bool Fixed = false;
  };
  std::map<LocalKey, Tally> Locals;
  std::function<void(const ExprPtr &)> Count = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (const auto Key = localKey(*E)) {
      Tally &T = Locals[*Key];
      const uint16_t Size = E->Type ? E->Type->Size : E->Var.Size;
      // Only a plain 1-, 2-, 4- or 8-byte integer has a signedness to pick.
      if (!isPlainInteger(E->Type) ||
          (Size != 1 && Size != 2 && Size != 4 && Size != 8))
        T.Fixed = true;
    }
    for (size_t I = 0; I < E->Operands.size(); ++I) {
      const ExprPtr &Operand = E->Operands[I];
      if (!Operand)
        continue;
      if (const auto Key = localKey(*Operand)) {
        Tally &T = Locals[*Key];
        switch (operandReads(*E, I)) {
        case Reads::Signed:
          ++T.Signed;
          break;
        case Reads::Unsigned:
          ++T.Unsigned;
          break;
        case Reads::Neither:
          break;
        }
      }
      Count(Operand);
    }
    if (E->IndirectTarget)
      Count(E->IndirectTarget);
  };
  forEachStmt(Func.Body, [&](HighStmt &S) {
    forEachExpr(S, Count);
    // A local that receives wrapping arithmetic takes it without a cast
    // only when unsigned.
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val && wrapsUnsigned(*S.Val))
      if (const auto Key = localKey(*S.Dst))
        ++Locals[*Key].Unsigned;
  });

  std::function<void(const ExprPtr &)> Retype = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (const auto Key = localKey(*E)) {
      const auto It = Locals.find(*Key);
      if (It != Locals.end() && !It->second.Fixed &&
          It->second.Unsigned > It->second.Signed)
        E->Type = NdType::makeInt(E->Type ? E->Type->Size : E->Var.Size, false);
    }
    for (const ExprPtr &Operand : E->Operands)
      Retype(Operand);
    if (E->IndirectTarget)
      Retype(E->IndirectTarget);
  };
  // Wrapping arithmetic whose result an unsigned local receives, and the
  // arithmetic feeding it, is unsigned too: the writer then spells it with
  // no conversion back to a signed type.
  std::function<void(const ExprPtr &)> Unsign = [&](const ExprPtr &E) {
    if (!E || !wrapsUnsigned(*E) || !E->Type ||
        E->Type->Kind != NdTypeKind::Int || E->Type->IsEnum)
      return;
    if (E->Type->IsSigned)
      E->Type = NdType::makeInt(E->Type->Size, false);
    for (const ExprPtr &Operand : E->Operands)
      if (Operand && Operand->Type && Operand->Type->Size == E->Type->Size)
        Unsign(Operand);
  };
  forEachStmt(Func.Body, [&](HighStmt &S) {
    forEachExpr(S, Retype);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val && S.Dst->Type &&
        S.Dst->Type->Kind == NdTypeKind::Int && !S.Dst->Type->IsSigned &&
        localKey(*S.Dst))
      Unsign(S.Val);
  });
}

} // namespace neverd
