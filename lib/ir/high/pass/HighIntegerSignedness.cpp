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
/// signedness most of its uses read, and a memory read the signedness of
/// the one operation reading it, so the emitted C carries fewer casts; the
/// value bits, and therefore the semantics, do not change.
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
  // A conversion to an integer as wide as its operand changes only how the
  // bits read, and disappears when the operand already reads that way.
  auto Integer = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Int && !Type->IsEnum;
  };
  if (Parent.Kind == ExprKind::Cast && Index == 0 && Integer(Parent.Type) &&
      !Parent.Operands.empty() && Parent.Operands[0] &&
      Integer(Parent.Operands[0]->Type) &&
      Parent.Operands[0]->Type->Size == Parent.Type->Size)
    return Parent.Type->IsSigned ? Reads::Signed : Reads::Unsigned;
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

/// Bitwise operations leave the same bits whatever the signedness of their
/// operands, and C defines them on either.
bool isBitwise(const HighExpr &E) {
  return (E.Kind == ExprKind::BinOp &&
          (E.Op == NdOp::INT_AND || E.Op == NdOp::INT_OR ||
           E.Op == NdOp::INT_XOR)) ||
         (E.Kind == ExprKind::UnaryOp && E.Op == NdOp::INT_NOT);
}

bool isPlainInteger(const TypeRef &Type) {
  return !Type || (Type->Kind == NdTypeKind::Int && !Type->IsEnum);
}

/// A plain read of a 1-, 2-, 4- or 8-byte integer, whose C spelling names
/// its signedness and nothing else: `*(_QWORD *)p` or `*(_SQWORD *)p`.
bool isPlainIntegerLoad(const HighExpr &E) {
  if (E.Kind != ExprKind::Load || !E.Type || !isPlainInteger(E.Type) ||
      E.MemoryOrdering != NdMemoryOrdering::None ||
      E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const uint16_t Size = E.Type->Size;
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
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
  // A bitwise operation takes the signedness its reader wants, and passes it
  // on to the bitwise operations feeding it: `x - ((x >> 1) & 0x55..)` needs
  // no conversion either side of the mask.  Wrapping arithmetic it feeds
  // turns unsigned only, which is what keeps it free of overflow.
  std::function<void(const ExprPtr &, bool)> Settle = [&](const ExprPtr &E,
                                                          bool Signed) {
    if (!E || !E->Type || E->Type->Kind != NdTypeKind::Int || E->Type->IsEnum)
      return;
    if (!Signed && wrapsUnsigned(*E)) {
      Unsign(E);
      return;
    }
    // A left shift into the sign bit overflows a signed operand, so it turns
    // unsigned only; a logical right shift reads its operand unsigned
    // whatever the result is called.
    const bool LeftShift =
        E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_LEFT;
    const bool RightShift =
        E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_RIGHT;
    if ((LeftShift && !Signed) || RightShift) {
      if (E->Type->IsSigned != Signed)
        E->Type = NdType::makeInt(E->Type->Size, Signed);
      return;
    }
    if (!isBitwise(*E))
      return;
    if (E->Type->IsSigned != Signed)
      E->Type = NdType::makeInt(E->Type->Size, Signed);
    for (const ExprPtr &Operand : E->Operands)
      if (Operand && Operand->Type && Operand->Type->Size == E->Type->Size)
        Settle(Operand, Signed);
  };
  std::function<void(const ExprPtr &)> SettleOperands = [&](const ExprPtr &E) {
    if (!E)
      return;
    for (size_t I = 0; I < E->Operands.size(); ++I) {
      const ExprPtr &Operand = E->Operands[I];
      if (!Operand)
        continue;
      if (const Reads Wanted = operandReads(*E, I); Wanted != Reads::Neither)
        Settle(Operand, Wanted == Reads::Signed);
      // A narrowing keeps low bytes, which are the same in either signedness:
      // read them unsigned, as the operation feeding them is spelled.
      else if (I == 0 && Operand->Type && E->Type &&
               Operand->Type->Size > E->Type->Size &&
               (E->Kind == ExprKind::Cast ||
                (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
                 E->Operands.size() == 2 && E->Operands[1] &&
                 E->Operands[1]->Kind == ExprKind::Const &&
                 E->Operands[1]->ConstVal == 0)))
        Settle(Operand, /*Signed=*/false);
      SettleOperands(Operand);
    }
    if (E->IndirectTarget)
      SettleOperands(E->IndirectTarget);
  };
  // A memory read has a single reader, the operation it is an operand of,
  // and reads the signedness that operation wants: `*(_QWORD *)p + 8`.  A
  // read a local receives whole has the local's.
  std::function<void(const ExprPtr &)> RetypeLoads = [&](const ExprPtr &E) {
    if (!E)
      return;
    for (size_t I = 0; I < E->Operands.size(); ++I) {
      const ExprPtr &Operand = E->Operands[I];
      if (!Operand)
        continue;
      if (isPlainIntegerLoad(*Operand))
        if (const Reads Wanted = operandReads(*E, I); Wanted != Reads::Neither)
          Operand->Type =
              NdType::makeInt(Operand->Type->Size, Wanted == Reads::Signed);
      RetypeLoads(Operand);
    }
    if (E->IndirectTarget)
      RetypeLoads(E->IndirectTarget);
  };
  forEachStmt(Func.Body, [&](HighStmt &S) {
    forEachExpr(S, Retype);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val && S.Dst->Type &&
        S.Dst->Type->Kind == NdTypeKind::Int && !S.Dst->Type->IsSigned &&
        localKey(*S.Dst))
      Unsign(S.Val);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val && S.Dst->Type &&
        isPlainInteger(S.Dst->Type) && localKey(*S.Dst) && S.Val->Type &&
        S.Val->Type->Size == S.Dst->Type->Size && isBitwise(*S.Val))
      Settle(S.Val, S.Dst->Type->IsSigned);
    forEachExpr(S, SettleOperands);
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val && localKey(*S.Dst) &&
        isPlainIntegerLoad(*S.Val) && isPlainInteger(S.Dst->Type) &&
        S.Dst->Type && S.Dst->Type->Size == S.Val->Type->Size)
      S.Val->Type = NdType::makeInt(S.Val->Type->Size, S.Dst->Type->IsSigned);
    forEachExpr(S, RetypeLoads);
  });
}

} // namespace neverd
