//===- HighNarrowLocals.cpp - Declare locals as wide as they are read -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A register local is as wide as its machine register, eight bytes on a
/// 64-bit target, yet code often uses only its low bytes: a 32-bit result
/// the processor zero-extends into the register is read back as 32 bits,
/// and each of those reads prints a narrowing conversion.  A local whose
/// every read takes at most its low N bytes, and whose every definition
/// extends a value of at most N bytes or is a constant, is declared N bytes
/// wide.  The bytes above, which no read sees, leave the program; the bytes
/// that remain do not change.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

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

bool isPlainInteger(const TypeRef &Type) {
  return Type && Type->Kind == NdTypeKind::Int && !Type->IsEnum;
}

uint16_t widthOf(const HighExpr &E) {
  return E.Type ? E.Type->Size : E.Var.Size;
}

/// How many low bytes \p Parent reads of its operand \p Index, or zero when
/// it reads the whole value.
uint16_t lowBytesRead(const HighExpr &Parent, size_t Index) {
  if (Index != 0 || !isPlainInteger(Parent.Type))
    return 0;
  if (Parent.Kind == ExprKind::Cast)
    return !Parent.CastTo || (isPlainInteger(Parent.CastTo) &&
                              Parent.CastTo->Size == Parent.Type->Size)
               ? Parent.Type->Size
               : 0;
  if (Parent.Kind == ExprKind::BinOp && Parent.Op == NdOp::SUBBYTES &&
      Parent.Operands.size() == 2 && Parent.Operands[1] &&
      Parent.Operands[1]->Kind == ExprKind::Const &&
      Parent.Operands[1]->ConstVal == 0)
    return Parent.Type->Size;
  return 0;
}

struct Local {
  uint16_t Width = 0;
  /// The widest read, which is how wide the local can be.
  uint16_t Read = 0;
  bool Keep = false;
  std::vector<HighStmt *> Definitions;
};

/// A value a narrowed local can still receive: an extension of at most
/// \p Width bytes, or a constant.  A constant that loses bytes must be a
/// number; an address cut short would name another place.  A call's result
/// would do as well, but the C writer recognizes a call statement by the
/// call at its top.
bool definesLowBytes(const HighExpr &Value, uint16_t Width) {
  if (Value.Kind == ExprKind::Const) {
    const uint64_t Mask = (uint64_t{1} << (8 * Width)) - 1;
    return isPlainInteger(Value.Type) &&
           ((Value.ConstVal & ~Mask) == 0 ||
            Value.ConstProvenance == ConstantAddressProvenance::Unknown ||
            Value.ConstProvenance == ConstantAddressProvenance::Scalar);
  }
  return Value.Kind == ExprKind::UnaryOp &&
         (Value.Op == NdOp::INT_ZEXT || Value.Op == NdOp::INT_SEXT) &&
         !Value.Operands.empty() && Value.Operands[0] &&
         isPlainInteger(Value.Operands[0]->Type) &&
         Value.Operands[0]->Type->Size <= Width;
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

bool narrowLocals(HighFunc &Func) {
  std::map<LocalKey, Local> Locals;
  auto Note = [&](const HighExpr &E) -> Local * {
    const auto Key = localKey(E);
    if (!Key)
      return nullptr;
    Local &L = Locals[*Key];
    L.Width = std::max(L.Width, widthOf(E));
    // A join, an incoming register or a value of another kind keeps its
    // declaration.  Renaming tags every defined register version and leaves
    // the incoming ones, version 0, untagged.
    if (E.Kind == ExprKind::Phi || !isPlainInteger(E.Type) ||
        (E.Var.Kind == MedVar::Reg && E.Var.SSAVer == 0 && E.Var.RenameTag < 0))
      L.Keep = true;
    return &L;
  };
  std::function<void(const ExprPtr &)> Reads = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (Local *L = Note(*E))
      L->Keep = true;
    for (size_t I = 0; I < E->Operands.size(); ++I) {
      const ExprPtr &Operand = E->Operands[I];
      if (!Operand)
        continue;
      if (const uint16_t Bytes = lowBytesRead(*E, I))
        if (localKey(*Operand) && Bytes < widthOf(*Operand)) {
          if (Local *L = Note(*Operand))
            L->Read = std::max(L->Read, Bytes);
          continue;
        }
      Reads(Operand);
    }
    if (E->IndirectTarget)
      Reads(E->IndirectTarget);
    for (const MedVar &Output : E->IntrinsicOutputs) {
      HighExpr Written;
      Written.Kind = ExprKind::Var;
      Written.Var = Output;
      if (const auto Key = localKey(Written))
        Locals[*Key].Keep = true;
    }
  };
  forEachStmt(Func.Body, [&](HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Dst->Kind == ExprKind::Var && localKey(*S.Dst)) {
      Local *L = Note(*S.Dst);
      L->Definitions.push_back(&S);
      Reads(S.Val);
      return;
    }
    forEachExpr(S, Reads);
  });

  // The narrowed locals, with the width each takes.
  std::map<LocalKey, uint16_t> Narrowed;
  for (const auto &[Key, L] : Locals) {
    if (L.Keep || L.Read == 0 || L.Read >= L.Width || L.Definitions.empty() ||
        (L.Read != 1 && L.Read != 2 && L.Read != 4))
      continue;
    bool Defined = true;
    for (const HighStmt *Def : L.Definitions)
      Defined &= definesLowBytes(*Def->Val, L.Read);
    if (Defined)
      Narrowed.emplace(Key, L.Read);
  }
  if (Narrowed.empty())
    return false;

  auto WidthFor = [&](const HighExpr &E) -> std::optional<uint16_t> {
    const auto Key = localKey(E);
    if (!Key)
      return std::nullopt;
    const auto It = Narrowed.find(*Key);
    if (It == Narrowed.end())
      return std::nullopt;
    return It->second;
  };
  auto Narrow = [&](HighExpr &E, uint16_t Width) {
    E.Var.Size = Width;
    E.Type = NdType::makeInt(Width, E.Type && E.Type->IsSigned);
  };
  for (const auto &[Key, Width] : Narrowed)
    for (HighStmt *Def : Locals[Key].Definitions) {
      const HighExpr &Value = *Def->Val;
      if (Value.Kind == ExprKind::Const) {
        const uint64_t Mask = (uint64_t{1} << (8 * Width)) - 1;
        Def->Val =
            HighExpr::makeConst(Value.ConstVal & Mask, Width,
                                Value.ConstProvenance, Value.AddressOwnerVA);
      } else if (Value.Operands[0]->Type->Size == Width) {
        Def->Val = Value.Operands[0];
      } else {
        ExprPtr Extended = HighExpr::makeUnary(Value.Op, Value.Operands[0]);
        Extended->Type = NdType::makeInt(Width, Value.Op == NdOp::INT_SEXT);
        Def->Val = Extended;
      }
    }
  std::function<void(const ExprPtr &)> Rewrite = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (const auto Width = WidthFor(*E))
      Narrow(*E, *Width);
    for (const ExprPtr &Operand : E->Operands)
      Rewrite(Operand);
    if (E->IndirectTarget)
      Rewrite(E->IndirectTarget);
  };
  forEachStmt(Func.Body, [&](HighStmt &S) { forEachExpr(S, Rewrite); });
  return true;
}

} // namespace neverd
