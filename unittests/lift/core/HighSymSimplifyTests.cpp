//===- HighSymSimplifyTests.cpp - Semantic simplification of HighIR -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins what the symbolic engine is allowed to do to a decompiled function,
/// from the other end: a MedIR function goes in and the C-shaped expression
/// comes out.
///
/// Two things are being tested at once, and both matter.  That the engine
/// reaches an expression at all — it only does once copy propagation has
/// folded the assignments into one tree — and that the translation across is
/// faithful in both directions, because a mistake there would not fail
/// loudly.  It would emit a smaller function that computes something else.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/ADT/APInt.h"

#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kEntry = 0x400000;
constexpr uint16_t kWordBytes = 4;

/// Builds a single-block function out of a sequence of operations on
/// temporaries, returning whatever the last one defined.
class FunctionBuilder {
public:
  FunctionBuilder() {
    Func.Entry = kEntry;
    Func.Name = "obfuscated";
    Func.ReturnType = NdType::makeInt(kWordBytes, false);
    Block.Id = 0;
    Block.StartAddr = kEntry;
  }

  /// A parameter, passed in a register so the converter names it as one.
  MedVar param(int Index, uint64_t RegOff) {
    MedVar V;
    V.Kind = MedVar::Param;
    V.TheArch = Arch::X64;
    V.Id = 100 + Index;
    V.SSAVer = 1;
    V.Size = kWordBytes;
    V.RegOff = RegOff;
    Func.Params.push_back(V);
    return V;
  }

  static MedVar constant(uint64_t Value) {
    return MedVar::makeConst(Value, kWordBytes);
  }

  MedVar emit(NdOp Op, std::vector<MedVar> Inputs) {
    MedVar Out;
    Out.Kind = MedVar::Temp;
    Out.TheArch = Arch::X64;
    Out.Id = NextTemp++;
    Out.SSAVer = 1;
    Out.Size = kWordBytes;
    return append(Op, std::move(Inputs), Out);
  }

  /// Emit the last operation into the return register and close the function.
  /// Return-value recovery looks for the final definition of that register
  /// rather than at what the RETURN names, so a temporary would leave the
  /// function returning nothing.
  HighFunc finish(NdOp Op, std::vector<MedVar> Inputs) {
    MedVar Out;
    Out.Kind = MedVar::Reg;
    Out.TheArch = Arch::X64;
    Out.Id = 900;
    Out.SSAVer = 1;
    Out.Size = kWordBytes;
    Out.RegOff = 0; // rax
    append(Op, std::move(Inputs), Out);

    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = NextAddr;
    Block.Ops.push_back(std::move(Return));
    Block.EndAddr = NextAddr + 4;
    Func.Blocks.push_back(std::move(Block));
    // convert() runs the production HighIR cleanup, including semantic
    // simplification.  Do not invoke the pass a second time here: these tests
    // must fail if the default decompilation path ever loses that integration.
    return MedToHighConverter().convert(Func, Arch::X64);
  }

private:
  MedVar append(NdOp Op, std::vector<MedVar> Inputs, MedVar Out) {
    MedOp Instr;
    Instr.Opcode = Op;
    Instr.Output = Out;
    Instr.Addr = NextAddr;
    NextAddr += 4;
    for (const MedVar &In : Inputs)
      Instr.addInput(In);
    Block.Ops.push_back(std::move(Instr));
    return Out;
  }

  MedFunc Func;
  MedBlock Block;
  int NextTemp = 1;
  va_t NextAddr = kEntry;
};

/// The returned expression, rendered the way HighIR prints it.
std::string returnedExpr(const HighFunc &Func) {
  for (const HighStmt &S : Func.Body)
    if (S.Kind == StmtKind::Return && S.RetVal)
      return S.RetVal->str();
  return "<no return>";
}

TEST(HighSymSimplify, RecoversAdditionFromItsBitwiseRewriting) {
  // `x + y`, written the way an obfuscator writes it.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38); // rdi
  MedVar Y = B.param(1, 0x30); // rsi
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});

  std::string Expr = returnedExpr(B.finish(NdOp::INT_ADD, {Xor, Scaled}));
  // Whichever way the operands come out ordered, neither the exclusive-or nor
  // the doubled conjunction may survive.
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, RecoversAdditionFromAConstantLeftShift) {
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_LEFT, {And, FunctionBuilder::constant(1)});

  const std::string Expr = returnedExpr(B.finish(NdOp::INT_ADD, {Xor, Scaled}));
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find("<<"), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, ReachesTheSmallestNontrivialIdentity) {
  // Four DAG nodes: x, ~x, 1 and the addition.  A size gate above four would
  // silently leave this supported identity out of the production HighIR path.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar NotX = B.emit(NdOp::INT_NOT, {X});

  std::string Expr = returnedExpr(
      B.finish(NdOp::INT_ADD, {NotX, FunctionBuilder::constant(1)}));
  EXPECT_EQ(Expr.find('~'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('+'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('-'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, RecoversExclusiveOrFromItsArithmeticRewriting) {
  // `(x | y) - (x & y)`.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Or = B.emit(NdOp::INT_OR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});

  std::string Expr = returnedExpr(B.finish(NdOp::INT_SUB, {Or, And}));
  EXPECT_NE(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('|'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, CollapsesAnExpressionThatIsSecretlyConstant) {
  // `(x & y) + (x | y) - x - y` is zero for every input.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Or = B.emit(NdOp::INT_OR, {X, Y});
  MedVar Sum = B.emit(NdOp::INT_ADD, {And, Or});
  MedVar LessX = B.emit(NdOp::INT_SUB, {Sum, X});

  EXPECT_EQ(returnedExpr(B.finish(NdOp::INT_SUB, {LessX, Y})), "0");
}

TEST(HighSymSimplify, FoldedScalarLiteralsRetainNumericProvenance) {
  using P = ConstantAddressProvenance;
  for (P FirstProvenance : {P::Scalar, P::Unknown}) {
    auto Color = HighExpr::makeBinop(
        NdOp::INT_OR,
        HighExpr::makeBinop(NdOp::INT_OR,
                            HighExpr::makeConst(0x3B0000, 8, FirstProvenance),
                            HighExpr::makeConst(0x5B00, 8, P::Scalar)),
        HighExpr::makeConst(0x1B, 8, P::Scalar));
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Color);
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    ASSERT_EQ(Body[0].RetVal->Kind, ExprKind::Const);
    EXPECT_EQ(Body[0].RetVal->ConstVal, 0x3B5B1BU);
    EXPECT_EQ(Body[0].RetVal->ConstProvenance, FirstProvenance);
  }
}

TEST(HighSymSimplify, LeavesAnOrdinaryExpressionAlone) {
  // Nothing here is hiding anything, and the pass must not churn it.
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Sum = B.emit(NdOp::INT_ADD, {X, Y});

  std::string Expr = returnedExpr(
      B.finish(NdOp::INT_LEFT, {Sum, FunctionBuilder::constant(3)}));
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
}

TEST(HighSymSimplify, AddressConstantsRetainTheirOriginAcrossNumericRewrites) {
  using P = ConstantAddressProvenance;
  for (P Provenance :
       {P::AddressFragment, P::Address, P::DataAddress, P::CodeAddress}) {
    auto Address = HighExpr::makeConst(0x401234, 8, Provenance, 0x401000);
    MedVar Input;
    Input.Kind = MedVar::Param;
    Input.Size = 8;
    auto X = HighExpr::makeVar(Input);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    auto Sum = HighExpr::makeBinop(
        NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, Address, X),
        HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(2, 8),
                            HighExpr::makeBinop(NdOp::INT_AND, Address, X)));
    Return.RetVal = HighExpr::makeBinop(NdOp::INT_SUB, Sum, X);
    std::vector<HighStmt> Body{Return};
    simplifyExprSemantics(Body);
    ASSERT_TRUE(Body[0].RetVal);
    EXPECT_TRUE(Body[0].RetVal->structuralEq(*Address));
    EXPECT_EQ(Body[0].RetVal->ConstProvenance, Provenance);
    EXPECT_EQ(Body[0].RetVal->AddressOwnerVA, 0x401000U);

    // Equal bits with a different meaning may not erase a relocatable input.
    auto Scalar = HighExpr::makeConst(0x401234, 8, P::Scalar);
    Return.RetVal = HighExpr::makeBinop(
        NdOp::INT_SUB, HighExpr::makeBinop(NdOp::INT_ADD, Address, X),
        HighExpr::makeBinop(NdOp::INT_ADD, Scalar, X));
    Body = {Return};
    simplifyExprSemantics(Body);
    std::vector<ExprPtr> Pending{Body[0].RetVal};
    bool RetainedAddress = false;
    while (!Pending.empty()) {
      auto E = Pending.back();
      Pending.pop_back();
      ASSERT_TRUE(E);
      RetainedAddress |= E->Kind == ExprKind::Const &&
                         E->ConstProvenance == Provenance &&
                         E->AddressOwnerVA == 0x401000;
      Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
    }
    EXPECT_TRUE(RetainedAddress);
  }
}

TEST(HighSymSimplify, KeepsWhatItCannotSeeInsideOf) {
  // A memory read contributes to the sum. It must survive as a snapshot at
  // its original statement while the surrounding arithmetic is simplified.
  FunctionBuilder B;
  MedVar Addr = B.param(0, 0x38);
  MedVar X = B.param(1, 0x30);
  MedVar Y = B.param(2, 0x10);
  MedVar Loaded = B.emit(NdOp::LOAD, {Addr});
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
  MedVar Sum = B.emit(NdOp::INT_ADD, {Xor, Scaled});

  HighFunc Func = B.finish(NdOp::INT_ADD, {Sum, Loaded});
  std::string Expr = returnedExpr(Func);
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;

  const HighStmt *Snapshot = nullptr;
  const HighStmt *Return = nullptr;
  unsigned ReadCount = 0;
  for (const HighStmt &Stmt : Func.Body) {
    if (Stmt.Kind == StmtKind::Assign && Stmt.Val &&
        Stmt.Val->Kind == ExprKind::Load) {
      EXPECT_EQ(Return, nullptr) << "memory read moved after its use";
      Snapshot = &Stmt;
      ++ReadCount;
    }
    if (Stmt.Kind == StmtKind::Return)
      Return = &Stmt;
  }
  ASSERT_EQ(ReadCount, 1U);
  ASSERT_NE(Snapshot, nullptr);
  ASSERT_TRUE(Snapshot->Dst);
  ASSERT_EQ(Snapshot->Val->Operands.size(), 1U);
  ASSERT_EQ(Snapshot->Val->Operands[0]->Kind, ExprKind::Var);
  EXPECT_EQ(Snapshot->Val->Operands[0]->Var.Kind, MedVar::Param);
  EXPECT_EQ(Snapshot->Val->Operands[0]->Var.Id, 0);
  EXPECT_EQ(Snapshot->Val->Operands[0]->Var.RegOff, Addr.RegOff);
  EXPECT_EQ(Snapshot->Val->Operands[0]->Var.Size, Addr.Size);
  ASSERT_TRUE(Snapshot->Val->Type);
  EXPECT_EQ(Snapshot->Val->Type->Size, kWordBytes);
  ASSERT_NE(Return, nullptr);
  bool UsesSnapshot = false;
  std::vector<ExprPtr> Worklist{Return->RetVal};
  while (!Worklist.empty()) {
    ExprPtr Current = Worklist.back();
    Worklist.pop_back();
    ASSERT_TRUE(Current);
    EXPECT_NE(Current->Kind, ExprKind::Load)
        << "return must use the saved value without repeating the read";
    UsesSnapshot |= Current->structuralEq(*Snapshot->Dst);
    Worklist.insert(Worklist.end(), Current->Operands.begin(),
                    Current->Operands.end());
  }
  EXPECT_TRUE(UsesSnapshot) << Expr;
}

TEST(HighSymSimplify, ReachesAnIdentityBelowSixtyFourExpressionLevels) {
  FunctionBuilder B;
  MedVar X = B.param(0, 0x38);
  MedVar Y = B.param(1, 0x30);
  MedVar Xor = B.emit(NdOp::INT_XOR, {X, Y});
  MedVar And = B.emit(NdOp::INT_AND, {X, Y});
  MedVar Scaled = B.emit(NdOp::INT_MULT, {And, FunctionBuilder::constant(2)});
  MedVar Wrapped = B.emit(NdOp::INT_ADD, {Xor, Scaled});

  // A hard recursion limit must not decide which parts of a real expression
  // the semantic pass can see.  The symbolic engine itself walks iteratively,
  // so this translator should be able to feed it a deep HighIR tree too.
  for (unsigned I = 0; I < 96; ++I)
    Wrapped =
        B.emit(NdOp::INT_ADD, {Wrapped, FunctionBuilder::constant(I + 1)});

  std::string Expr = returnedExpr(
      B.finish(NdOp::INT_ADD, {Wrapped, FunctionBuilder::constant(97)}));
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
}

//===----------------------------------------------------------------------===//
// Words wider than the machine's
//===----------------------------------------------------------------------===//

/// A temporary of \p Bytes bytes.  Building HighIR directly is what lets these
/// reach a width no x86 register has.
MedVar tempOfSize(int Id, uint16_t Bytes) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Bytes;
  return V;
}

/// \p E after the production semantic pass has had it.
ExprPtr simplified(ExprPtr E) {
  std::vector<HighStmt> Body;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(E);
  Body.push_back(std::move(Return));
  simplifyExprSemantics(Body);
  return Body.front().RetVal;
}

// The carry-save identity again, at a width no machine register has.  Nothing
// in the engine cares: its literals are arbitrary precision and a 128-bit word
// is measured exactly as a 32-bit one.  The only thing that ever left a
// `__int128` obfuscation standing was this bridge declining to carry one
// across.
TEST(HighSymSimplify, RecoversAdditionAtAWidthWiderThanAMachineWord) {
  constexpr uint16_t kWideBytes = 16;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr Carry = HighExpr::makeBinop(NdOp::INT_MULT,
                                      HighExpr::makeBinop(NdOp::INT_AND, X, Y),
                                      HighExpr::makeConst(2, kWideBytes));
  ExprPtr Result = simplified(HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, X, Y), Carry));

  const std::string Expr = Result->str();
  EXPECT_EQ(Expr.find('^'), std::string::npos) << Expr;
  EXPECT_EQ(Expr.find('&'), std::string::npos) << Expr;
  EXPECT_NE(Expr.find('+'), std::string::npos) << Expr;
  // What comes back has to be the width that went in, or the backend would
  // print a 128-bit value through a narrower type.
  ASSERT_TRUE(Result->Type);
  EXPECT_EQ(Result->Type->Size, kWideBytes);
}

// The value has to survive the round trip as well as the width.  This is zero
// at every input, and only a measurement carried out in arbitrary precision
// can say so at 256 bits.
TEST(HighSymSimplify, CollapsesAWideExpressionThatIsSecretlyConstant) {
  constexpr uint16_t kWideBytes = 32;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr Sum = HighExpr::makeBinop(NdOp::INT_ADD,
                                    HighExpr::makeBinop(NdOp::INT_AND, X, Y),
                                    HighExpr::makeBinop(NdOp::INT_OR, X, Y));
  ExprPtr Result = simplified(HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeBinop(NdOp::INT_SUB, Sum, X), Y));

  EXPECT_EQ(Result->str(), "0");
}

/// The carry-save spelling of `x + y`, less both of its inputs again, so what
/// the engine is left holding is whatever constant tail was folded in.
ExprPtr wideConstantTail(const ExprPtr &X, const ExprPtr &Y, ExprPtr Tail,
                         uint16_t Bytes) {
  ExprPtr Carry = HighExpr::makeBinop(NdOp::INT_MULT,
                                      HighExpr::makeBinop(NdOp::INT_AND, X, Y),
                                      HighExpr::makeConst(2, Bytes));
  ExprPtr Sum = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, X, Y), Carry);
  ExprPtr Whole =
      HighExpr::makeBinop(NdOp::INT_ADD, std::move(Sum), std::move(Tail));
  return HighExpr::makeBinop(NdOp::INT_SUB,
                             HighExpr::makeBinop(NdOp::INT_SUB, Whole, X), Y);
}

// Measuring derives values HighIR has no room to write down: a literal is kept
// in sixty-four bits whatever size stands beside it, and a 128-bit all-ones
// needs all of them.  Keeping the low half would be a smaller expression
// computing something else, so the value is spelled as the negation it is
// instead -- which is exact, and is what a reader wanted to see anyway.
TEST(HighSymSimplify, SpellsAWideConstantTooLargeToStoreAsANegation) {
  // `(x ^ y) + 2 * (x & y) + (-1) - x - y` is -1 for every input.
  constexpr uint16_t kWideBytes = 16;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr MinusOne =
      HighExpr::makeUnary(NdOp::INT_NEG2, HighExpr::makeConst(1, kWideBytes));
  ExprPtr Result =
      simplified(wideConstantTail(X, Y, std::move(MinusOne), kWideBytes));

  EXPECT_EQ(Result->str(), "-1");
  ASSERT_TRUE(Result->Type);
  EXPECT_EQ(Result->Type->Size, kWideBytes);
}

// A value with no exact spelling at all is the case that has to be refused.
// `1 << 100` is not a literal HighIR can hold, and neither its negation nor its
// complement is one either, so there is nothing to write down -- and writing
// its low sixty-four bits would be a shorter expression computing something
// else, which is the one thing this pass may never hand back.  What comes out
// is therefore the very object that went in.
TEST(HighSymSimplify, DeclinesARewriteWhoseConstantHasNoSpelling) {
  constexpr uint16_t kWideBytes = 16;
  ExprPtr X = HighExpr::makeVar(tempOfSize(1, kWideBytes));
  ExprPtr Y = HighExpr::makeVar(tempOfSize(2, kWideBytes));
  ExprPtr Shifted =
      HighExpr::makeBinop(NdOp::INT_LEFT, HighExpr::makeConst(1, kWideBytes),
                          HighExpr::makeConst(100, kWideBytes));
  ExprPtr Before = wideConstantTail(X, Y, std::move(Shifted), kWideBytes);

  EXPECT_EQ(simplified(Before).get(), Before.get()) << Before->str();
}

ExprPtr integerCast(ExprPtr Operand, uint16_t Bytes, bool Signed = false) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Cast;
  E->Type = E->CastTo = NdType::makeInt(Bytes, Signed);
  E->Operands = {std::move(Operand)};
  return E;
}

ExprPtr extendInteger(ExprPtr Operand, uint16_t Bytes, bool Signed = false) {
  auto E = HighExpr::makeUnary(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT,
                               std::move(Operand));
  E->Type = NdType::makeInt(Bytes, false);
  return E;
}

ExprPtr carrySum(const ExprPtr &X, const ExprPtr &Y) {
  return HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeBinop(NdOp::INT_XOR, X, Y),
      HighExpr::makeBinop(NdOp::INT_MULT, HighExpr::makeConst(2, X->Type->Size),
                          HighExpr::makeBinop(NdOp::INT_AND, X, Y)));
}

// Evaluate exact bitvector operations independently of the symbolic bridge.
// The small-domain tests below include every input, including all carry and
// sign transitions; wide products retain their full APInt intermediate.
llvm::APInt
integerValue(const ExprPtr &E, uint64_t X, uint64_t Y = 0,
             const std::unordered_map<int, uint64_t> *Bindings = nullptr) {
  const unsigned Width = E->Type->Size * 8;
  if (E->Kind == ExprKind::Const)
    return llvm::APInt(Width, E->ConstVal, false, true);
  if (E->Kind == ExprKind::Var) {
    if (Bindings) {
      auto It = Bindings->find(E->Var.Id);
      if (It == Bindings->end()) {
        ADD_FAILURE() << "read before definition in test interpreter";
        return llvm::APInt(Width, 0);
      }
      return llvm::APInt(Width, It->second, false, true);
    }
    return llvm::APInt(Width, E->Var.Id == 1 ? X : Y, false, true);
  }
  llvm::APInt A = integerValue(E->Operands[0], X, Y, Bindings);
  if (E->Kind == ExprKind::Cast)
    return E->Operands[0]->Type->IsSigned ? A.sextOrTrunc(Width)
                                          : A.zextOrTrunc(Width);
  if (E->Kind == ExprKind::UnaryOp) {
    switch (E->Op) {
    case NdOp::INT_ZEXT:
      return A.zextOrTrunc(Width);
    case NdOp::INT_SEXT:
      return A.sextOrTrunc(Width);
    case NdOp::INT_NOT:
    case NdOp::INT_NEGATE:
      return ~A;
    case NdOp::INT_NEG2:
      return -A;
    default:
      ADD_FAILURE() << "unsupported unary test expression";
      return llvm::APInt(Width, 0);
    }
  }
  if (E->Op == NdOp::SUBBYTES)
    return A.extractBits(Width, E->Operands[1]->ConstVal * 8);
  llvm::APInt B =
      integerValue(E->Operands[1], X, Y, Bindings).zextOrTrunc(Width);
  A = A.zextOrTrunc(Width);
  switch (E->Op) {
  case NdOp::INT_ADD:
    return A + B;
  case NdOp::INT_SUB:
    return A - B;
  case NdOp::INT_MULT:
    return A * B;
  case NdOp::INT_AND:
    return A & B;
  case NdOp::INT_OR:
    return A | B;
  case NdOp::INT_XOR:
    return A ^ B;
  default:
    ADD_FAILURE() << "unsupported binary test expression";
    return llvm::APInt(Width, 0);
  }
}

TEST(HighSymSimplify, RecoversSubtractionThroughWidenedLowProducts) {
  for (uint16_t Bytes : {1, 2, 4, 8}) {
    SCOPED_TRACE(Bytes);
    const auto X = HighExpr::makeVar(tempOfSize(1, Bytes));
    const auto Y = HighExpr::makeVar(tempOfSize(2, Bytes));
    const auto Coefficient = extendInteger(
        HighExpr::makeConst(llvm::APInt::getAllOnes(Bytes * 8).getZExtValue(),
                            Bytes),
        Bytes * 2);
    const auto Product = HighExpr::makeBinop(NdOp::INT_MULT, Coefficient,
                                             extendInteger(Y, Bytes * 2));
    for (bool UseSlice : {false, true}) {
      ExprPtr Low = integerCast(Product, Bytes);
      if (UseSlice) {
        Low = HighExpr::makeBinop(NdOp::SUBBYTES, Product,
                                  HighExpr::makeConst(0, 4));
        Low->Type = NdType::makeInt(Bytes, false);
      }
      const auto Difference = HighExpr::makeBinop(NdOp::INT_ADD, X, Low);
      const auto Before =
          HighExpr::makeBinop(NdOp::INT_AND, Difference,
                              HighExpr::makeBinop(NdOp::INT_OR, X, Difference));
      const auto After = simplified(Before);
      const auto Text = After->str();
      EXPECT_EQ(Text.find('*'), std::string::npos) << Text;
      EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
      EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
      EXPECT_NE(Text.find('-'), std::string::npos) << Text;
      for (uint64_t A : {0ULL, 1ULL, 0x7fULL, 0x80ULL, ~0ULL})
        for (uint64_t B : {0ULL, 1ULL, 0xffULL, 0x8000000000000000ULL, ~0ULL})
          EXPECT_EQ(integerValue(After, A, B), integerValue(Before, A, B));
    }
  }
}

TEST(HighSymSimplify, WideningUsesTheSourceSignedness) {
  for (bool SourceSigned : {false, true}) {
    const auto X =
        HighExpr::makeVar(tempOfSize(1, 1), NdType::makeInt(1, SourceSigned));
    const auto Y =
        HighExpr::makeVar(tempOfSize(2, 1), NdType::makeInt(1, false));
    const auto Before = carrySum(integerCast(X, 2), extendInteger(Y, 2));
    const auto After = simplified(Before);
    EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
    for (uint64_t A = 0; A < 256; ++A)
      for (uint64_t B : {0ULL, 1ULL, 127ULL, 128ULL, 255ULL})
        EXPECT_EQ(integerValue(After, A, B), integerValue(Before, A, B));
    EXPECT_EQ(X->Type->IsSigned, SourceSigned);
  }
}

TEST(HighSymSimplify, NarrowArithmeticKeepsItsCarryBoundaryWhenWidened) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 1));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 1));
  const auto Before = HighExpr::makeBinop(
      NdOp::INT_SUB, extendInteger(carrySum(X, Y), 2),
      HighExpr::makeBinop(NdOp::INT_ADD, extendInteger(X, 2),
                          extendInteger(Y, 2)));
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  for (uint64_t A = 0; A < 256; ++A)
    for (uint64_t B = 0; B < 256; ++B)
      ASSERT_EQ(integerValue(After, A, B).getZExtValue(),
                A + B > 255 ? 0xff00U : 0U);
}

TEST(HighSymSimplify, HighProductSliceKeepsCarriesFromTheLowByte) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 1));
  const auto Product = HighExpr::makeBinop(NdOp::INT_MULT, extendInteger(X, 2),
                                           HighExpr::makeConst(255, 2));
  auto High =
      HighExpr::makeBinop(NdOp::SUBBYTES, Product, HighExpr::makeConst(1, 4));
  High->Type = NdType::makeInt(1, false);
  const auto Before = carrySum(High, X);
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  for (uint64_t A = 0; A < 256; ++A)
    EXPECT_EQ(integerValue(After, A), integerValue(Before, A));
}

TEST(HighSymSimplify, TruncationBeforeWideningStillDropsUpperBits) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 2));
  const auto Low = integerCast(X, 1, true);
  const auto Before = carrySum(extendInteger(Low, 2, true), X);
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  for (uint64_t A = 0; A < 65536; ++A)
    ASSERT_EQ(integerValue(After, A), integerValue(Before, A));
}

TEST(HighSymSimplify, SignedLiteralCastsPreserveTheOriginalSignBit) {
  auto Negative = HighExpr::makeConst(255, 1);
  Negative->Type = NdType::makeInt(1, true);
  const auto X = HighExpr::makeVar(tempOfSize(1, 2));
  const auto Before = carrySum(integerCast(Negative, 2), X);
  const auto After = simplified(Before);
  for (uint64_t A : {0ULL, 1ULL, 255ULL, 32768ULL, 65535ULL})
    EXPECT_EQ(integerValue(After, A).getZExtValue(), (A - 1) & 65535);
}

TEST(HighSymSimplify, CastsDoNotHideEffectsUnknownValuesOrPointerIdentity) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 8));
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->Type = NdType::makeInt(4, false);
  Call->CallTarget = "read_value";
  auto Pointer = HighExpr::makeVar(tempOfSize(2, 8));
  Pointer->Type = NdType::makePtr(NdType::makeInt(1, false));
  auto Float = HighExpr::makeConst(0, 4);
  Float->Type = NdType::makeFloat(4);
  const auto Quotient = HighExpr::makeBinop(
      NdOp::INT_DIV, X, HighExpr::makeVar(tempOfSize(2, 8)));
  auto Invalid = integerCast(HighExpr::makeConst(1, 4), 8);
  Invalid->CastTo = NdType::makeInt(2, false);
  auto InvalidExtension = extendInteger(X, 4);
  auto InvalidSlice =
      HighExpr::makeBinop(NdOp::SUBBYTES, X, HighExpr::makeConst(8, 4));
  InvalidSlice->Type = NdType::makeInt(4, false);
  auto InvalidBitCast = std::make_shared<HighExpr>();
  InvalidBitCast->Kind = ExprKind::BitCast;
  InvalidBitCast->Type = NdType::makeInt(4, false);
  InvalidBitCast->Operands = {X};
  for (const auto &Value :
       {Call, HighExpr::makeLoad(X, NdType::makeInt(4, false)),
        HighExpr::makeUndef(4), Pointer, Float, Quotient, Invalid,
        InvalidExtension, InvalidSlice, InvalidBitCast}) {
    const auto Opaque = integerCast(Value, 8);
    const auto Before = carrySum(Opaque, X);
    EXPECT_EQ(simplified(Before).get(), Before.get());
  }
}

TEST(HighSymSimplify, ARecoveredVariableRetainsItsOtherUsesSignedness) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4), NdType::makeInt(4, true));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4), NdType::makeInt(4, true));
  const auto Before = HighExpr::makeBinop(NdOp::INT_SUB, carrySum(X, Y), Y);
  Before->Type = NdType::makeInt(4, false);
  const auto After = simplified(Before);
  ASSERT_EQ(After->Kind, ExprKind::Cast);
  EXPECT_FALSE(After->Type->IsSigned);
  EXPECT_FALSE(After->CastTo->IsSigned);
  EXPECT_EQ(After->Operands[0].get(), X.get());
  EXPECT_TRUE(X->Type->IsSigned);
  EXPECT_TRUE(Y->Type->IsSigned);
}

TEST(HighSymSimplify, ReachesNumericRegionsUnderOpaquePredicateOperations) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto OriginalSum = carrySum(X, Y);
  const auto Population = HighExpr::makeUnary(NdOp::POPCOUNT, OriginalSum);
  const auto Predicate = HighExpr::makeBinop(NdOp::INT_EQUAL, Population,
                                             HighExpr::makeConst(3, 4));
  const auto After = simplified(Predicate);
  EXPECT_EQ(After->Kind, ExprKind::BinOp);
  EXPECT_EQ(After->Op, NdOp::INT_EQUAL);
  ASSERT_EQ(After->Operands.size(), 2U);
  EXPECT_EQ(After->Operands[0]->Op, NdOp::POPCOUNT);
  EXPECT_EQ(After->str().find('^'), std::string::npos) << After->str();
  EXPECT_EQ(After->str().find('&'), std::string::npos) << After->str();
  EXPECT_NE(OriginalSum->str().find('^'), std::string::npos);
}

TEST(HighSymSimplify, SimplifyingCallArgumentsPreservesTheCallAndOrderedLoad) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto Load = HighExpr::makeLoad(X, NdType::makeInt(4, false),
                                       NdMemoryOrdering::Acquire);
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->Type = NdType::makeInt(4, false);
  Call->CallTarget = "observe_values";
  Call->CallAddr = 0x401000;
  Call->Operands = {carrySum(X, Y), Load};
  const auto After = simplified(Call);
  EXPECT_EQ(After->Kind, ExprKind::Call);
  EXPECT_EQ(After->CallTarget, Call->CallTarget);
  EXPECT_EQ(After->CallAddr, Call->CallAddr);
  ASSERT_EQ(After->Operands.size(), 2U);
  EXPECT_EQ(After->Operands[1].get(), Load.get());
  EXPECT_EQ(After->Operands[0]->str().find('^'), std::string::npos);
  EXPECT_EQ(Call->Operands[0]->Op, NdOp::INT_ADD);
  EXPECT_NE(Call->Operands[0]->str().find('^'), std::string::npos);
}

TEST(HighSymSimplify, SimplifyingNumericPointerOperandKeepsThePointerCast) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 8));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 8));
  auto Cast = integerCast(carrySum(X, Y), 8);
  Cast->Type = Cast->CastTo = NdType::makePtr();
  const auto After = simplified(Cast);
  EXPECT_EQ(After->Kind, ExprKind::Cast);
  EXPECT_EQ(After->Type.get(), Cast->Type.get());
  EXPECT_EQ(After->CastTo.get(), Cast->CastTo.get());
  EXPECT_EQ(After->Operands[0]->str().find('^'), std::string::npos);
}

TEST(HighSymSimplify, KeepsRecoveredSliceWidthWhenTheRootHasNoType) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 8));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto Low = integerCast(X, 4);
  auto Sum = carrySum(Low, Y);
  Sum->Type.reset();
  auto Before = HighExpr::makeBinop(NdOp::INT_SUB, Sum, Y);
  const auto After = simplified(Before);
  ASSERT_EQ(After->Kind, ExprKind::Cast);
  ASSERT_TRUE(After->Type);
  ASSERT_TRUE(After->CastTo);
  EXPECT_EQ(After->Type->Size, 4U);
  EXPECT_EQ(After->CastTo->Size, 4U);
  EXPECT_EQ(After->Operands[0].get(), X.get());
}

TEST(HighSymSimplify, InfersWidthsOnceAcrossAnUnannotatedDeepDAG) {
  std::vector<ExprPtr> Nodes;
  auto Unannotated = [&](ExprPtr E) {
    E->Type.reset();
    Nodes.push_back(E);
    return E;
  };
  const auto X = Unannotated(HighExpr::makeVar(tempOfSize(1, 4)));
  const auto Y = Unannotated(HighExpr::makeVar(tempOfSize(2, 4)));
  const auto Xor = Unannotated(HighExpr::makeBinop(NdOp::INT_XOR, X, Y));
  const auto And = Unannotated(HighExpr::makeBinop(NdOp::INT_AND, X, Y));
  const auto Scaled = Unannotated(
      HighExpr::makeBinop(NdOp::INT_MULT, And, HighExpr::makeConst(2, 4)));
  auto Before = Unannotated(HighExpr::makeBinop(NdOp::INT_ADD, Xor, Scaled));
  for (unsigned I = 0; I < 8192; ++I)
    Before = Unannotated(
        HighExpr::makeBinop(NdOp::INT_ADD, Before, HighExpr::makeConst(0, 4)));
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('^'), std::string::npos);
  EXPECT_EQ(After->str().find('&'), std::string::npos);
  EXPECT_NE(After->str().find('+'), std::string::npos);
  // Teardown must not itself recurse down the synthetic stress chain.
  for (auto &E : Nodes)
    E->Operands.clear();
}

HighStmt assignValue(const ExprPtr &Destination, const ExprPtr &Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = Destination;
  S.Val = Value;
  return S;
}

HighStmt returnValue(const ExprPtr &Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = Value;
  return S;
}

ExprPtr sharedMaskUse(const ExprPtr &X, const ExprPtr &Y, const ExprPtr &Mask) {
  return HighExpr::makeBinop(NdOp::INT_ADD, X,
                             HighExpr::makeBinop(NdOp::INT_OR, Mask, Y));
}

TEST(HighSymSimplify, UsesVisiblePureDefinitionsWithoutExpandingTheOutput) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto Mask = HighExpr::makeBinop(NdOp::INT_AND, X, Y);
  std::vector<HighStmt> Body{assignValue(T, Mask),
                             returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body.front().Val.get(), Mask.get());
  const auto Text = Body.back().RetVal->str();
  EXPECT_EQ(Text.find('|'), std::string::npos) << Text;
  EXPECT_EQ(Text.find('&'), std::string::npos) << Text;
  EXPECT_NE(Text.find('+'), std::string::npos) << Text;
}

TEST(HighSymSimplify, InvalidatesDefinitionsAcrossRenamedLocalWrites) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  X->Var.RenameTag = 7;
  const auto Alias = HighExpr::makeVar(tempOfSize(90, 4));
  Alias->Var.RenameTag = 7;
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto U = HighExpr::makeVar(tempOfSize(4, 4));
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
      assignValue(U, T), assignValue(Alias, HighExpr::makeConst(17, 4)),
      returnValue(sharedMaskUse(X, Y, U))};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos)
      << Body.back().RetVal->str();
}

TEST(HighSymSimplify, ReassigningATemporaryDoesNotReuseItsOldDefinition) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
      assignValue(T, HighExpr::makeBinop(NdOp::INT_ADD, T, X)),
      returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos)
      << Body.back().RetVal->str();
}

TEST(HighSymSimplify, AvailableDefinitionsNeverGrowTheOriginalUse) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto Before = HighExpr::makeBinop(NdOp::INT_AND, T, X);
  std::vector<HighStmt> Body{assignValue(T, carrySum(X, Y)),
                             returnValue(Before)};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body.back().RetVal.get(), Before.get());
  EXPECT_EQ(Body.back().RetVal->Operands[0].get(), T.get());
}

TEST(HighSymSimplify, DoesNotCarryAvailableDefinitionsAcrossControlFlow) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  const auto Definition =
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y));
  HighStmt Jump;
  Jump.Kind = StmtKind::Goto;
  Jump.GotoTarget = 0x402000;
  auto Return = returnValue(sharedMaskUse(X, Y, T));
  Return.Addr = Jump.GotoTarget;
  std::vector<HighStmt> WithLabel{Jump, Definition, Return};
  simplifyExprSemantics(WithLabel);
  EXPECT_NE(WithLabel.back().RetVal->str().find('|'), std::string::npos);

  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = sharedMaskUse(X, Y, T);
  Loop.Body.push_back(assignValue(T, X));
  std::vector<HighStmt> WithLoop{Definition, Loop};
  simplifyExprSemantics(WithLoop);
  EXPECT_NE(WithLoop.back().Cond->str().find('|'), std::string::npos);

  auto PhiDefinition = Definition;
  PhiDefinition.IsPhiCopy = true;
  std::vector<HighStmt> WithPhi{PhiDefinition, Return};
  simplifyExprSemantics(WithPhi);
  EXPECT_NE(WithPhi.back().RetVal->str().find('|'), std::string::npos);
}

TEST(HighSymSimplify, AddressTakenLocalsNeverBecomeSymbolicDefinitions) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = NdType::makePtr();
  Address->Operands = {T};
  HighStmt Escape;
  Escape.Kind = StmtKind::ExprStmt;
  Escape.Val = Address;
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
      returnValue(sharedMaskUse(X, Y, T)), Escape};
  simplifyExprSemantics(Body);
  EXPECT_NE(Body[1].RetVal->str().find('|'), std::string::npos);
  EXPECT_EQ(Body.back().Val.get(), Address.get());
  EXPECT_EQ(Address->Operands[0].get(), T.get());
}

TEST(HighSymSimplify, EffectBoundariesInvalidateAvailableDefinitions) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->Type = NdType::makeInt(4);
  Call->CallTarget = "update_state";
  HighStmt Effect;
  Effect.Kind = StmtKind::ExprStmt;
  Effect.CallExpr = Call;
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)), Effect,
      returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body[1].CallExpr.get(), Call.get());
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos);
}

TEST(HighSymSimplify, DefinitionBindingsRequireMatchingIntegerTypes) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  for (uint16_t Bytes : {4, 8}) {
    const auto T =
        HighExpr::makeVar(tempOfSize(3, Bytes), NdType::makeInt(Bytes, false));
    const auto Use = Bytes == 4 ? T : integerCast(T, 4);
    std::vector<HighStmt> Body{
        assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)),
        returnValue(sharedMaskUse(X, Y, Use))};
    simplifyExprSemantics(Body);
    EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos)
        << Body.back().RetVal->str();
  }
}

TEST(HighSymSimplify, ImplicitOutputsInvalidateAvailableDefinitions) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  auto ImplicitWrite = HighExpr::makeUnary(NdOp::INT_NOT, X);
  ImplicitWrite->IntrinsicOutputs.push_back(X->Var);
  HighStmt Effect;
  Effect.Kind = StmtKind::ExprStmt;
  Effect.Val = ImplicitWrite;
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_AND, X, Y)), Effect,
      returnValue(sharedMaskUse(X, Y, T))};
  simplifyExprSemantics(Body);
  EXPECT_EQ(Body[1].Val.get(), ImplicitWrite.get());
  EXPECT_EQ(Body[1].Val->IntrinsicOutputs.size(), 1U);
  EXPECT_NE(Body.back().RetVal->str().find('|'), std::string::npos);
}

TEST(HighSymSimplify, DefinitionSnapshotsKeepValuesBeforeInputReassignment) {
  const auto X = HighExpr::makeVar(tempOfSize(1, 4));
  const auto Y = HighExpr::makeVar(tempOfSize(2, 4));
  const auto T = HighExpr::makeVar(tempOfSize(3, 4));
  std::vector<HighStmt> Body{
      assignValue(T, HighExpr::makeBinop(NdOp::INT_ADD, X, Y)),
      assignValue(X, HighExpr::makeConst(7, 4)),
      returnValue(HighExpr::makeBinop(
          NdOp::INT_SUB, HighExpr::makeBinop(NdOp::INT_SUB, T, X), Y))};
  simplifyExprSemantics(Body);
  for (uint64_t A : {0ULL, 1ULL, 7ULL, 127ULL, 0x80000000ULL, 0xffffffffULL})
    for (uint64_t B : {0ULL, 17ULL, 0xffffffffULL}) {
      std::unordered_map<int, uint64_t> Values{{1, A}, {2, B}};
      for (const auto &S : Body) {
        if (S.Kind == StmtKind::Assign)
          Values[S.Dst->Var.Id] =
              integerValue(S.Val, 0, 0, &Values).getZExtValue();
        else if (S.Kind == StmtKind::Return)
          EXPECT_EQ(integerValue(S.RetVal, 0, 0, &Values).getZExtValue(),
                    (A - 7) & 0xffffffffU);
      }
    }
}

ExprPtr parameterView(int Id, uint16_t Bytes) {
  auto V = tempOfSize(Id, Bytes);
  V.Kind = MedVar::Param;
  return HighExpr::makeVar(V, NdType::makeInt(Bytes, false));
}

TEST(HighSymSimplify, RelatesNarrowParameterViewsToTheObservedWideValue) {
  const auto Wide = parameterView(1, 8);
  const auto Low = parameterView(1, 4);
  const auto Y = parameterView(2, 4);
  const auto Difference = integerCast(
      HighExpr::makeBinop(NdOp::INT_SUB, Wide, HighExpr::makeConst(1, 8)), 4);
  const auto Before = HighExpr::makeBinop(
      NdOp::INT_SUB, Difference,
      HighExpr::makeBinop(NdOp::INT_OR, HighExpr::makeUnary(NdOp::INT_NOT, Y),
                          Low));
  const auto After = simplified(Before);
  EXPECT_EQ(After->str().find('-'), std::string::npos) << After->str();
  EXPECT_EQ(After->str().find('~'), std::string::npos) << After->str();
  EXPECT_NE(After->str().find('|'), std::string::npos) << After->str();
  for (uint64_t A : {0ULL, 0xffffffffULL, 0x1234567880000000ULL, ~0ULL})
    for (uint64_t B : {0ULL, 1ULL, 0x80000000ULL, 0xffffffffULL})
      EXPECT_EQ(integerValue(After, A, B).getZExtValue(),
                (A | B) & 0xffffffffU);
}

TEST(HighSymSimplify, ParameterViewRelationsNeverInventUpperBits) {
  const auto Wide = parameterView(1, 8);
  const auto Low = parameterView(1, 4);
  for (bool Signed : {false, true}) {
    const auto Before =
        HighExpr::makeBinop(NdOp::INT_SUB, Wide, extendInteger(Low, 8, Signed));
    const auto After = simplified(Before);
    for (uint64_t A : {0ULL, 0x80000000ULL, 0x1234567880000000ULL,
                       0xffffffff00000000ULL, ~0ULL})
      EXPECT_EQ(integerValue(After, A), integerValue(Before, A));
    EXPECT_NE(integerValue(After, 0x1234567800000000ULL).getZExtValue(), 0U);
  }
}

TEST(HighSymSimplify, DoesNotMergeUnrelatedOrMalformedParameterViews) {
  const auto Wide = parameterView(1, 8);
  auto DifferentId = parameterView(2, 4);
  auto DifferentVersion = parameterView(1, 4);
  ++DifferentVersion->Var.SSAVer;
  auto OrdinaryLocal = HighExpr::makeVar(tempOfSize(1, 4));
  auto Malformed = parameterView(1, 8);
  Malformed->Type = NdType::makeInt(4, false);
  for (const auto &Other :
       {DifferentId, DifferentVersion, OrdinaryLocal, Malformed}) {
    const auto After = simplified(
        HighExpr::makeBinop(NdOp::INT_SUB, integerCast(Wide, 4), Other));
    EXPECT_NE(After->str(), "0") << After->str();
  }
}

TEST(HighCDeadStoreAnalysis, WalksDeepExpressionGraphsOnAWorkerStack) {
  MedVar Input;
  Input.Kind = MedVar::Param;
  Input.TheArch = Arch::X64;
  Input.Id = 1;
  Input.SSAVer = 1;
  Input.Size = kWordBytes;

  ExprPtr Deep = HighExpr::makeVar(Input);
  for (unsigned I = 0; I < 8192; ++I)
    Deep = HighExpr::makeBinop(NdOp::INT_ADD, std::move(Deep),
                               HighExpr::makeConst(I, kWordBytes));

  HighFunc Func;
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Deep);
  Func.Body.push_back(std::move(Return));

  HighCAnalysisState State;
  std::thread Worker([&] {
    analyzeDeadStores(
        State, Func, [](const MedVar &V) { return V.display(); },
        [](const HighExpr &) { return std::string("expression"); });
  });
  Worker.join();
  EXPECT_TRUE(State.DeadStmts.empty());

  // Shared-pointer destruction follows the same chain recursively.  Dismantle
  // this synthetic stress tree iteratively so the test measures the analysis,
  // not the standard library's control-block teardown.
  ExprPtr Current = std::move(Func.Body.front().RetVal);
  while (Current && Current->Kind == ExprKind::BinOp &&
         !Current->Operands.empty()) {
    ExprPtr Next = std::move(Current->Operands[0]);
    Current->Operands.clear();
    Current = std::move(Next);
  }
}

} // namespace
