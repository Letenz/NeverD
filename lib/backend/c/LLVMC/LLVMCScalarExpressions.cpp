//===- LLVMCScalarExpressions.cpp - Typed integer source expressions ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMCWriter.h"

namespace neverd {
namespace {

// Bits is the LLVM value width; CBits/Unsigned describe the C expression,
// before integer promotion. Every expression represents its normalized LLVM
// bit pattern as a nonnegative C value. These are distinct properties: a
// zero-extension can keep a narrower C type, and a comparison has type int.
struct ScalarExpression {
  std::string Text;
  unsigned Bits;
  unsigned CBits;
  bool Unsigned;
  unsigned Precedence;
};

constexpr unsigned Primary = 16, Unary = 15, Multiply = 13, Add = 12,
                   Shift = 11, Relational = 10, Equality = 9, BitAnd = 8,
                   BitXor = 7, BitOr = 6, Conditional = 3;

std::string operand(const ScalarExpression &E, unsigned Prec,
                    bool Right = false) {
  return E.Precedence < Prec || (Right && E.Precedence == Prec)
             ? "(" + E.Text + ")"
             : E.Text;
}

ScalarExpression unsignedCast(ScalarExpression E, unsigned Bits) {
  if (Bits == 1) {
    const std::string Text = operand(E, BitAnd) + " & 1u";
    if (E.CBits > 32)
      return {"(uint32_t)(" + Text + ")", 1, 32, true, Unary};
    return {Text, 1, 32, true, BitAnd};
  }
  E.Text = "(uint" + std::to_string(Bits) + "_t)" + operand(E, Unary);
  E.Bits = E.CBits = Bits;
  E.Unsigned = true;
  E.Precedence = Unary;
  return E;
}

// All signed C operands admitted here are known nonnegative (narrow values,
// literal leaves and comparisons). Their promoted type still matters for
// arithmetic: multiplying two uint16_t objects as signed int can overflow.
std::pair<unsigned, bool> commonType(const ScalarExpression &L,
                                     const ScalarExpression &R) {
  const unsigned LW = std::max(32u, L.CBits), RW = std::max(32u, R.CBits);
  const bool LU = L.CBits >= 32 && L.Unsigned;
  const bool RU = R.CBits >= 32 && R.Unsigned;
  return {std::max(LW, RW), LW == RW ? LU || RU : LW > RW ? LU : RU};
}

ScalarExpression binary(ScalarExpression L, ScalarExpression R, const char *Op,
                        unsigned Prec, unsigned Bits) {
  const auto [Width, Unsigned] = commonType(L, R);
  return {operand(L, Prec) + " " + Op + " " + operand(R, Prec, true), Bits,
          Width, Unsigned, Prec};
}

} // namespace

std::optional<std::string>
LLVMCWriter::scalarExpressionText(const llvm::Value *V, bool ForceExpression,
                                  bool InvertCompare) {
  using Expr = std::optional<ScalarExpression>;
  unsigned Work = 128;
  auto Render = [&](auto &&Self, const llvm::Value *Value, unsigned Depth,
                    bool Force) -> Expr {
    if (!Value || Depth > 32 || Work == 0 || !Value->getType()->isIntegerTy())
      return std::nullopt;
    --Work;
    const unsigned Bits = Value->getType()->getIntegerBitWidth();
    if (Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64)
      return std::nullopt;
    if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(Value)) {
      const std::string Suffix = Bits == 64 ? "ull" : Bits == 32 ? "u" : "";
      return ScalarExpression{std::to_string(C->getZExtValue()) + Suffix, Bits,
                              Bits == 64 ? 64u : 32u, Bits >= 32, Primary};
    }
    const auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value);
    if (llvm::isa<llvm::Argument>(Value) ||
        (Inst && !Force &&
         (MaterializedExpressions.count(Inst) ||
          !Analysis.Inlinable.count(Inst)))) {
      ScalarExpression E{getName(Value), Bits, Bits == 1 ? 8u : Bits, true,
                         Primary};
      // The C ABI uses a byte carrier for i1. Preserve the LLVM low bit even
      // when the C object has a wider representation.
      return Bits == 1 ? unsignedCast(std::move(E), 1) : E;
    }
    if (!Inst || llvm::isa<llvm::PHINode>(Inst))
      return std::nullopt;
    auto Child = [&](unsigned Index) {
      return Self(Self, Inst->getOperand(Index), Depth + 1, false);
    };
    if (const auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Inst)) {
      auto L = Child(0), R = Child(1);
      if (!L || !R)
        return std::nullopt;
      const auto Pred = Value == V && InvertCompare
                            ? Compare->getInversePredicate()
                            : Compare->getPredicate();
      if (llvm::CmpInst::isSigned(Pred)) {
        if (L->Bits == 1)
          return std::nullopt;
        for (auto *E : {&*L, &*R}) {
          E->Text =
              "(int" + std::to_string(E->Bits) + "_t)" + operand(*E, Unary);
          E->Precedence = Unary;
        }
      }
      const char *Op = nullptr;
      switch (Pred) {
      case llvm::CmpInst::ICMP_EQ:
        Op = "==";
        break;
      case llvm::CmpInst::ICMP_NE:
        Op = "!=";
        break;
      case llvm::CmpInst::ICMP_UGT:
      case llvm::CmpInst::ICMP_SGT:
        Op = ">";
        break;
      case llvm::CmpInst::ICMP_UGE:
      case llvm::CmpInst::ICMP_SGE:
        Op = ">=";
        break;
      case llvm::CmpInst::ICMP_ULT:
      case llvm::CmpInst::ICMP_SLT:
        Op = "<";
        break;
      case llvm::CmpInst::ICMP_ULE:
      case llvm::CmpInst::ICMP_SLE:
        Op = "<=";
        break;
      default:
        return std::nullopt;
      }
      auto E =
          binary(*L, *R, Op, Compare->isEquality() ? Equality : Relational, 1);
      E.CBits = 32;
      E.Unsigned = false;
      return E;
    }
    if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      auto C = Child(0), L = Child(1), R = Child(2);
      if (!C || !L || !R)
        return std::nullopt;
      const auto [Width, Unsigned] = commonType(*L, *R);
      return ScalarExpression{operand(*C, Conditional + 1) + " ? " +
                                  operand(*L, Conditional, true) + " : " +
                                  operand(*R, Conditional),
                              Bits, Width, Unsigned, Conditional};
    }
    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      auto E = Child(0);
      if (!E)
        return std::nullopt;
      if (Cast->getOpcode() == llvm::Instruction::ZExt) {
        E->Bits = Bits;
        return E;
      }
      if (Cast->getOpcode() == llvm::Instruction::Trunc)
        return unsignedCast(*E, Bits);
      if (Cast->getOpcode() == llvm::Instruction::SExt && E->Bits != 1) {
        E->Text = "(int" + std::to_string(E->Bits) + "_t)" + operand(*E, Unary);
        E->Precedence = Unary;
        return unsignedCast(*E, Bits);
      }
      return std::nullopt;
    }
    if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(Inst)) {
      if (Call->getIntrinsicID() != llvm::Intrinsic::fshl &&
          Call->getIntrinsicID() != llvm::Intrinsic::fshr)
        return std::nullopt;
      std::string Text = functionIdentifier(*Call->getCalledFunction()) + "(";
      for (unsigned I = 0; I != 3; ++I) {
        auto Arg = Child(I);
        if (!Arg)
          return std::nullopt;
        if (I)
          Text += ", ";
        Text += Arg->Text;
      }
      return ScalarExpression{Text + ")", Bits, Bits == 1 ? 8u : Bits, true,
                              Primary};
    }
    const auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(Inst);
    if (!BO)
      return std::nullopt;
    auto L = Child(0), R = Child(1);
    if (!L || !R)
      return std::nullopt;
    const unsigned Op = BO->getOpcode();
    const char *Token = nullptr;
    unsigned Prec = 0;
    switch (Op) {
    case llvm::Instruction::Add:
      Token = "+";
      Prec = Add;
      break;
    case llvm::Instruction::Sub:
      Token = "-";
      Prec = Add;
      break;
    case llvm::Instruction::Mul:
      Token = "*";
      Prec = Multiply;
      break;
    case llvm::Instruction::And:
      Token = "&";
      Prec = BitAnd;
      break;
    case llvm::Instruction::Or:
      Token = "|";
      Prec = BitOr;
      break;
    case llvm::Instruction::Xor:
      Token = "^";
      Prec = BitXor;
      break;
    case llvm::Instruction::Shl:
      Token = "<<";
      Prec = Shift;
      break;
    case llvm::Instruction::LShr:
      Token = ">>";
      Prec = Shift;
      break;
    default:
      return std::nullopt;
    }
    const bool Arithmetic = Op == llvm::Instruction::Add ||
                            Op == llvm::Instruction::Sub ||
                            Op == llvm::Instruction::Mul;
    if (Arithmetic && (Bits >= 32 || Op == llvm::Instruction::Mul)) {
      const unsigned Carrier = std::max(32u, Bits);
      const auto [Width, Unsigned] = commonType(*L, *R);
      if (!Unsigned || Width != Carrier)
        L = unsignedCast(*L, Carrier);
    }
    if (Op == llvm::Instruction::Shl &&
        (!L->Unsigned || L->CBits < std::max(32u, Bits)))
      L = unsignedCast(*L, std::max(32u, Bits));
    // A zero extension may keep a narrow C representation, but a shift by
    // more than that representation's width still needs the LLVM carrier.
    if (Op == llvm::Instruction::LShr && L->CBits < Bits && Bits > 32)
      L = unsignedCast(*L, Bits);
    auto E = binary(*L, *R, Token, Prec, Bits);
    if (Op == llvm::Instruction::Shl || Op == llvm::Instruction::LShr) {
      E.CBits = std::max(32u, L->CBits);
      E.Unsigned = L->CBits >= 32 && L->Unsigned;
    }
    if (Bits < 32 && (Arithmetic || Op == llvm::Instruction::Shl))
      return unsignedCast(std::move(E), Bits);
    return E;
  };
  auto E = Render(Render, V, 0, ForceExpression);
  if (!E)
    return std::nullopt;
  return operand(*E, Primary);
}

} // namespace neverd
