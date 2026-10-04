//===- LLVMScalarModelTests.cpp - Scalar interface and arithmetic oracles -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarEquivalenceTest.h"

namespace neverd::analysis::scalar_test {
TEST(LLVMScalarModel, IntegerInputsAndResultsPreserveDeclaredWidths) {
  for (unsigned Bits : {1U, 8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const std::string T = "i" + std::to_string(Bits);
    Source S("define " + T + " @f(" + T + " noundef %x, " + T +
             " noundef %y) { %r = xor " + T + " %x, %y\nret " + T + " %r }");
    ASSERT_TRUE(S.Module);
    auto M = modelLLVMScalarFunction(S.function());
    ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
    EXPECT_EQ(M->ResultBits, Bits);
    ASSERT_EQ(M->Arguments.size(), 2U);
    EXPECT_EQ(M->Arguments[0].Bits, Bits);
    EXPECT_EQ(M->Arguments[0].Storage.Size, (Bits + 7) / 8);
    auto A = llvm::APInt::getAllOnes(Bits), B = llvm::APInt(Bits, 0x95);
    auto R = evaluate(*M, {A, B});
    ASSERT_TRUE(R);
    EXPECT_EQ(*R, (A ^ B).zext((Bits + 7) / 8 * 8));
    EXPECT_EQ(checkLLVMScalarEquivalence(S.function(), S.function()).Status,
              Status::Proved);
  }
}

TEST(LLVMScalarModel, FunnelResultsMatchDoubleWidthConcatenation) {
  for (unsigned Bits : {1U, 8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const std::string T = "i" + std::to_string(Bits);
    for (bool Left : {false, true}) {
      SCOPED_TRACE(Left);
      const std::string Name =
          std::string("llvm.") + (Left ? "fshl." : "fshr.") + T;
      Source S("declare " + T + " @" + Name + "(" + T + ", " + T + ", " + T +
               ")\ndefine " + T + " @f(" + T + " noundef %a, " + T +
               " noundef %b, " + T + " noundef %n) { %r = call noundef " + T +
               " @" + Name + "(" + T + " %a, " + T + " %b, " + T +
               " %n)\nret " + T + " %r }");
      ASSERT_TRUE(S.Module);
      auto M = modelLLVMScalarFunction(S.function());
      ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
      for (uint64_t Raw = 0; Raw < Bits * 2 + 2; ++Raw) {
        // Include count zero, exact multiples, distinct operands, high raw
        // count bits and signed-looking data. The oracle works on a joined
        // double word rather than the model's two shifts and conditional.
        for (uint64_t High : {uint64_t{0}, uint64_t{1} << (Bits - 1)}) {
          llvm::APInt A(Bits, 0xa79135bdf24680e1ULL);
          llvm::APInt B(Bits, 0x184fe6239a57cd02ULL);
          llvm::APInt N(Bits, Raw | High);
          const unsigned Count = N.getZExtValue() % Bits;
          auto Pair = (A.zext(Bits * 2).shl(Bits) | B.zext(Bits * 2));
          auto Expected = Left ? Pair.shl(Count).lshr(Bits).trunc(Bits)
                               : Pair.lshr(Count).trunc(Bits);
          auto Actual = evaluate(*M, {A, B, N});
          ASSERT_TRUE(Actual);
          EXPECT_EQ(*Actual, Expected.zext((Bits + 7) / 8 * 8));
        }
      }
    }
  }
}

TEST(LLVMScalarModel, GuardedProductsCheckBothSignsAndFullHighWord) {
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const std::string T = "i" + std::to_string(Bits);
    const llvm::APInt Values[] = {llvm::APInt(Bits, 0),
                                  llvm::APInt(Bits, 1),
                                  llvm::APInt(Bits, 3),
                                  llvm::APInt::getSignedMinValue(Bits),
                                  llvm::APInt::getSignedMaxValue(Bits),
                                  llvm::APInt::getAllOnes(Bits)};
    for (const char *Flags : {"nuw", "nsw", "nuw nsw"}) {
      SCOPED_TRACE(Flags);
      Source S("define " + T + " @f(" + T + " noundef %x, " + T +
               " noundef %y) { %r = mul " + Flags + " " + T + " %x, %y\nret " +
               T + " %r }");
      ASSERT_TRUE(S.Module);
      auto M = modelLLVMScalarFunction(S.function());
      ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
      for (auto A : Values)
        for (auto B : Values) {
          bool UnsignedOverflow = false, SignedOverflow = false;
          auto Expected = A.umul_ov(B, UnsignedOverflow);
          (void)A.smul_ov(B, SignedOverflow);
          bool Defined =
              (!llvm::StringRef(Flags).contains("nuw") || !UnsignedOverflow) &&
              (!llvm::StringRef(Flags).contains("nsw") || !SignedOverflow);
          auto R = evaluate(*M, {A, B}, Defined);
          ASSERT_TRUE(R);
          EXPECT_EQ(*R, Expected);
        }
    }
  }
}

TEST(LLVMScalarModel, UnsupportedEffectsAndContractsCannotBecomeScalarProofs) {
  const char *Cases[] = {
      "define i32 @f(ptr noundef %x) { %v = load i32, ptr %x\nret i32 %v }",
      "define i32 @f(i32 noundef %x) { %p = alloca i32\nret i32 %x }",
      "define i32 @f(i32 noundef %x) { %p = inttoptr i32 %x to ptr\nret i32 %x "
      "}",
      "define i32 @f(i32 noundef %x) { ret i32 undef }",
      "define i32 @f(i32 noundef %x) { ret i32 poison }",
      "define i32 @f(i32 noundef %x) { %v = freeze i32 %x\nret i32 %v }",
      "define i32 @f(i32 noundef %x) { %v = udiv i32 %x, 3\nret i32 %v }",
      "define i128 @f(i128 noundef %x) { ret i128 %x }",
      "define i7 @f(i7 noundef %x) { ret i7 %x }",
      "define fastcc i32 @f(i32 noundef %x) { ret i32 %x }",
      "define i32 @f(i32 noundef %x, ...) { ret i32 %x }",
      "define i32 @f(i32 noundef %x) \"unknown-contract\" { ret i32 %x }",
      "declare i32 @g(i32) memory(none)\n"
      "define i32 @f(i32 noundef %x) { %r = call i32 @g(i32 %x)\nret i32 %r }",
      "declare i32 @llvm.fshl.i32(i32, i32, i32)\n"
      "define i32 @f(i32 noundef %x) { %r = call range(i32 0, 8) i32 "
      "@llvm.fshl.i32(i32 %x, i32 %x, i32 0)\nret i32 %r }"};
  for (const char *IR : Cases) {
    SCOPED_TRACE(IR);
    auto R = check(IR, IR);
    EXPECT_EQ(R.Status, Status::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.Diagnostic.empty());
    EXPECT_EQ(R.CompletedPartitions, 0U);
  }
}

TEST(LLVMScalarModel, ReturnRangesRetainTheirDefinednessObligation) {
  constexpr char Good[] = R"(
define range(i8 0, 4) i8 @f(i8 noundef %x) {
 %r = and i8 %x, 3
 ret i8 %r
})";
  EXPECT_EQ(check(Good, Good).Status, Status::Proved);
  std::string Bad = Good;
  Bad.replace(Bad.find("0, 4"), 4, "0, 3");
  EXPECT_EQ(check(Good, Bad).Status, Status::Unproved);
}

TEST(LLVMScalarModel, ArithmeticDependencyCarriesCannotBeLost) {
  // The observed top bit after addition depends on every lower input bit,
  // including the rare all-ones carry. A single representative would lie.
  constexpr char Carry[] = R"(
define i8 @f(i8 noundef %x) {
 %sum = add i8 %x, 1
 %high = lshr i8 %sum, 7
 %c = icmp eq i8 %high, 0
 br i1 %c, label %a, label %b
a: ret i8 0
b: ret i8 1
})";
  constexpr char Wrong[] = "define i8 @f(i8 noundef %x) { ret i8 0 }";
  auto Self = check(Carry, Carry);
  ASSERT_EQ(Self.Status, Status::Proved) << Self.Diagnostic;
  EXPECT_EQ(Self.CompletedPartitions, 256U);
  EXPECT_EQ(check(Carry, Wrong).Status, Status::Unproved);
}
} // namespace neverd::analysis::scalar_test
