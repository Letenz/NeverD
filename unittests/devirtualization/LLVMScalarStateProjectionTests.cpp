//===- LLVMScalarStateProjectionTests.cpp - Explicit state interfaces -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "neverd/analysis/LLVMScalarStateProjection.h"
#include "neverd/analysis/arch/x86_64/InterpreterMachineState.h"
#include "neverd/analysis/arch/x86_64/LLVMScalarStateProjection.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

#include <cstring>

namespace neverd::analysis::scalar_test {
using StateProjection = LLVMScalarStateProjectionResult;
static constexpr char Layout[] = "target datalayout = \"e-p:64:64\"\n";

static StateProjection
projectState(Source &S, const LLVMScalarStateContract &C,
             const LLVMScalarStateProjectionLimits &L = {}) {
  const auto Before = S.text();
  auto R = projectLLVMScalarState(S.function(), C, L);
  EXPECT_EQ(Before, S.text());
  if (R.Module)
    EXPECT_FALSE(llvm::verifyModule(*R.Module));
  return R;
}

static LLVMScalarResultProjectionResult field(StateProjection &R,
                                              unsigned Index, unsigned Bits) {
  return projectLLVMScalarResult(*R.Module->getFunction("f"),
                                 {{Index}, 0, Bits});
}

static void equivalent(StateProjection &R, unsigned Index, unsigned Bits,
                       llvm::StringRef IR) {
  SCOPED_TRACE(Index);
  auto P = field(R, Index, Bits);
  ASSERT_TRUE(P.Module) << P.Diagnostic;
  Source Oracle(IR);
  auto Proof = checkLLVMScalarEquivalence(*P.Module->getFunction("f"),
                                          Oracle.function());
  EXPECT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
}

TEST(LLVMScalarStateProjection,
     OverlappingAccessesAndObserversRetainAllInputs) {
  Source S(std::string(Layout) + R"(
define i64 @f(ptr noundef %s) {
  %whole = load i64, ptr %s, align 8
  %p = getelementptr i8, ptr %s, i64 2
  store i16 4660, ptr %p, align 2
  %v = load i32, ptr %s, align 4
  %high = getelementptr i8, ptr %s, i64 4
  %carry = lshr i64 %whole, 32
  %keep = trunc i64 %carry to i32
  store i32 %keep, ptr %high, align 4
  ret i64 0
})");
  LLVMScalarStateContract C{
      8, 32, {}, {{0, 32, false}, {4, 32, true}, {1, 32, false}}};
  auto R = projectState(S, C);
  ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
  EXPECT_EQ(R.Arguments, 2U);
  EXPECT_EQ(R.Loads, 2U);
  EXPECT_EQ(R.Stores, 2U);
  equivalent(R, 1, 32, R"(
define i32 @f(i32 noundef %x, i32 noundef %y) {
 %lo = and i32 %x, 65535
 %r = or i32 %lo, 305397760
 ret i32 %r
})");
  equivalent(R, 2, 32,
             "define i32 @f(i32 noundef %x, i32 noundef %y) {ret i32 0}");
  // A shifted OR can remain outside the bounded symbolic normalizer. The
  // compiled byte oracle below covers this unaligned observation directly.
  auto Unaligned = field(R, 3, 32);
  ASSERT_TRUE(Unaligned.Module) << Unaligned.Diagnostic;
  for (auto &B : *R.Module->getFunction("f"))
    for (auto &I : B) {
      EXPECT_FALSE((llvm::isa<llvm::LoadInst, llvm::StoreInst, llvm::AllocaInst,
                              llvm::GetElementPtrInst>(I)));
    }
}

static constexpr char Loop[] = R"(
target datalayout = "e-p:64:64"
define range(i64 0, 3) i64 @f(ptr noundef %s) {
entry:
  %p = getelementptr i8, ptr %s, i64 4
  %n = load i32, ptr %s, align 4
  %count = and i32 %n, 3
  %empty = icmp eq i32 %count, 0
  br i1 %empty, label %zero, label %loop
zero: ret i64 0
loop:
  %i = phi i32 [0, %entry], [%next, %loop]
  %v = load i32, ptr %p, align 4
  %sum = add i32 %v, 5
  store i32 %sum, ptr %p, align 4
  %next = add nuw i32 %i, 1
  %more = icmp ult i32 %next, %count
  br i1 %more, label %loop, label %exit
exit: ret i64 1
})";

TEST(LLVMScalarStateProjection, LoopPhiMasksAndMultipleReturns) {
  Source S(Loop);
  LLVMScalarStateContract C{
      8, 32, {{0, 7, 0}}, {{4, 32, false}, {0, 32, true}}};
  auto R = projectState(S, C);
  ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
  equivalent(R, 1, 32, R"(
define i32 @f(i32 noundef %n, i32 noundef %v) {
 %count = and i32 %n, 3
 %delta = mul i32 %count, 5
 %r = add i32 %v, %delta
 ret i32 %r
})");
  equivalent(R, 2, 32,
             "define i32 @f(i32 noundef %x, i32 noundef %y) {ret i32 0}");
  equivalent(R, 0, 64, R"(
define i64 @f(i32 noundef %x, i32 noundef %y) {
 %n = and i32 %x, 3
 %c = icmp ne i32 %n, 0
 %r = zext i1 %c to i64
 ret i64 %r
})");
}

TEST(LLVMScalarStateProjection, EntryDifferenceUsesEffectiveMaskedState) {
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    Source S(std::string(Layout) + "define i64 @f(ptr noundef %s) {ret i64 0}");
    LLVMScalarStateContract C{
        Bits / 8, Bits, {{0, 0, 7}}, {{0, Bits, false}, {0, Bits, true}}};
    auto R = projectState(S, C);
    ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
    const auto Ty = "i" + std::to_string(Bits);
    for (unsigned Index : {1U, 2U})
      equivalent(R, Index, Bits,
                 "define " + Ty + " @f(" + Ty + " noundef %x) {ret " + Ty +
                     (Index == 1 ? " 7}" : " 0}"));
  }
}

TEST(LLVMScalarStateProjection,
     OmittedStateAndStatusRemainDefinednessObligations) {
  for (const char *Operation :
       {"%bad = add nuw i8 255, 1", "call void @llvm.assume(i1 false)"}) {
    Source S(std::string(Layout) +
             "declare void @llvm.assume(i1)\n"
             "define i64 @f(ptr noundef %s) {\n" +
             Operation + "\nstore i8 1, ptr %s, align 1\nret i64 0}");
    auto R = projectState(S, {8, 32, {}, {}});
    ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
    auto P = field(R, 0, 64);
    ASSERT_TRUE(P.Module) << P.Diagnostic;
    auto &F = *P.Module->getFunction("f");
    EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Unproved);
  }
  for (const char *Range : {"range(i64 0, 3)", "range(i64 -2, 2)"})
    for (const char *Value : {"1", "4"}) {
      Source S(std::string(Layout) + "define " + Range +
               " i64 @f(ptr noundef %s) {ret i64 " + Value + "}");
      auto R = projectState(S, {8, 32, {}, {{0, 8, false}}});
      ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
      // Even observing only the untouched state must prove the status range.
      auto P = field(R, 1, 8);
      ASSERT_TRUE(P.Module) << P.Diagnostic;
      auto &F = *P.Module->getFunction("f");
      EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status,
                std::string(Value) == "1" ? Status::Proved : Status::Unproved);
    }
}

TEST(LLVMScalarStateProjection,
     UnsupportedMemoryPointersAndContractsAreAtomic) {
  const char *Bodies[] = {
      "%bad = shl i8 3, 8\nret i64 0",
      "%p = load i64, ptr %s, align 8\n%q = inttoptr i64 %p to ptr\n"
      "%v = load i8, ptr %q, align 1\nret i64 0",
      "%v = load volatile i8, ptr %s, align 1\nret i64 0",
      "%v = load atomic i8, ptr %s unordered, align 1\nret i64 0",
      "%v = load i8, ptr %s, align 1, !range !0\nret i64 0",
      "%p = ptrtoint ptr %s to i64\nret i64 %p",
      "%p = getelementptr i8, ptr %s, i64 8\nret i64 0",
      "%p = getelementptr i8, ptr %s, i64 1\n"
      "%v = load i64, ptr %p, align 1\nret i64 %v",
      "%v = load i64, ptr %s, align 16\nret i64 %v",
      "%v = load i64, ptr %s, align 8\n"
      "%p = getelementptr i8, ptr %s, i64 %v\n"
      "%r = load i8, ptr %p, align 1\nret i64 0"};
  for (const char *Body : Bodies) {
    SCOPED_TRACE(Body);
    Source S(std::string(Layout) + "define i64 @f(ptr noundef %s) {\n" + Body +
             "}\n!0 = !{i8 0, i8 2}");
    auto R = projectState(S, {8, 32, {}, {}});
    EXPECT_EQ(R.Status, StateProjection::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.Module);
  }
  for (const char *DL : {"E-p:64:64", "e-p:32:32", "e-p:64:64-ni:1"}) {
    // Non-integral address space 1 is unrelated; only used address spaces
    // matter.
    Source S(std::string("target datalayout = \"") + DL +
             "\"\ndefine i64 @f(ptr noundef %s) {ret i64 0}");
    auto R = projectState(S, {8, 32, {}, {}});
    EXPECT_EQ(R.Status, std::string(DL) == "e-p:64:64-ni:1"
                            ? StateProjection::Projected
                            : StateProjection::Unsupported)
        << R.Diagnostic;
  }
}

TEST(LLVMScalarStateProjection,
     ExplicitLimitsAndMalformedContractsPublishNothing) {
  Source S(Loop);
  const LLVMScalarStateContract C{8, 32, {}, {{4, 32, false}}};
  auto R = projectState(S, C);
  ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
  LLVMScalarStateProjectionLimits L;
  L.MaxConstructionWork = R.ConstructionWork;
  EXPECT_EQ(projectState(S, C, L).Status, StateProjection::Projected);
  --L.MaxConstructionWork;
  EXPECT_EQ(projectState(S, C, L).Status, StateProjection::BudgetExceeded);
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    L = {};
    if (Kind == 0)
      L.MaxStateBytes = 7;
    if (Kind == 1)
      L.MaxArguments = 1;
    if (Kind == 2)
      L.MaxObservations = 0;
    if (Kind == 3)
      L.Model.MaxWork = 0;
    auto Limited = projectState(S, C, L);
    EXPECT_EQ(Limited.Status, StateProjection::BudgetExceeded);
    EXPECT_FALSE(Limited.Module);
  }
  const LLVMScalarStateContract Bad[] = {{0, 32, {}, {}},
                                         {7, 32, {}, {}},
                                         {8, 24, {}, {}},
                                         {8, 32, {{2, 0, 0}}, {}},
                                         {8, 32, {{0, UINT64_MAX, 0}}, {}},
                                         {8, 32, {{0, 0, 0}, {0, 1, 0}}, {}},
                                         {8, 32, {}, {{8, 8, false}}},
                                         {8, 32, {}, {{1, 64, false}}},
                                         {8, 32, {}, {{0, 7, false}}}};
  for (const auto &Invalid : Bad) {
    auto Refused = projectState(S, Invalid);
    EXPECT_EQ(Refused.Status, StateProjection::Unsupported)
        << Refused.Diagnostic;
    EXPECT_FALSE(Refused.Module);
  }
}

TEST(LLVMScalarStateProjection, ReprojectionChecksChangedUnobservedArithmetic) {
  Source S(std::string(Layout) + R"(
define i64 @f(ptr noundef %s) {
 %v = load i8, ptr %s, align 1
 %dead = add nuw i8 %v, 0
 ret i64 0
}
define i8 @unrelated() {ret i8 7}
)");
  auto First = projectState(S, {8, 64, {}, {}});
  ASSERT_EQ(First.Status, StateProjection::Projected) << First.Diagnostic;
  EXPECT_FALSE(First.Module->getFunction("unrelated"));
  auto Before = field(First, 0, 64);
  ASSERT_TRUE(Before.Module);
  auto &F = *Before.Module->getFunction("f");
  EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Proved);
  for (auto &I : S.function().front())
    if (auto *Add = llvm::dyn_cast<llvm::BinaryOperator>(&I))
      Add->setOperand(
          1, llvm::ConstantInt::get(llvm::Type::getInt8Ty(S.Context), 1));
  auto Changed = projectState(S, {8, 64, {}, {}});
  ASSERT_EQ(Changed.Status, StateProjection::Projected) << Changed.Diagnostic;
  auto After = field(Changed, 0, 64);
  ASSERT_TRUE(After.Module);
  auto &G = *After.Module->getFunction("f");
  EXPECT_EQ(checkLLVMScalarEquivalence(G, G).Status, Status::Unproved);
  EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Proved);
}

TEST(LLVMScalarStateProjection, X64DomainUsesTheMachineStateProfile) {
  const auto Profile = InterpreterMachineStateProfile::UserX64NoFaultV1;
  auto Plain = llvmScalarStateContractX64(Profile, {});
  ASSERT_TRUE(bool(Plain));
  EXPECT_EQ(Plain->StateBytes, sizeof(InterpreterMachineStateX64V1));
  EXPECT_EQ(Plain->CellBits, 32U);
  EXPECT_EQ(Plain->Entry.size(), 2U);
  EXPECT_TRUE(Plain->Observations.empty());
  auto Aligned = llvmScalarStateContractX64(Profile, {{0, 64, false}},
                                            InterpreterEntryAlignment{16, 8});
  ASSERT_TRUE(bool(Aligned));
  EXPECT_EQ(Aligned->Observations.size(), 1U);
  uint64_t Seed = 0xc51a79ed409e263b;
  for (unsigned N = 0; N < 2048; ++N) {
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    for (const auto *Contract : {&*Plain, &*Aligned}) {
      uint32_t Cells[34];
      for (auto &Cell : Cells)
        Cell = uint32_t(Seed);
      for (auto Mask : Contract->Entry)
        Cells[Mask.Cell] = (Cells[Mask.Cell] & Mask.And) | Mask.Or;
      InterpreterMachineStateX64V1 State;
      std::memcpy(&State, Cells, sizeof(State));
      auto E = validateInterpreterMachineStateX64V1(State);
      EXPECT_FALSE(bool(E));
      llvm::consumeError(std::move(E));
      if (Contract == &*Aligned)
        EXPECT_EQ(State.GPR[4] % 16, 8U);
      else
        EXPECT_EQ(uint32_t(State.GPR[4]), uint32_t(Seed));
    }
  }
  auto Invalid =
      llvmScalarStateContractX64(Profile, {}, InterpreterEntryAlignment{3, 0});
  EXPECT_FALSE(bool(Invalid));
  llvm::consumeError(Invalid.takeError());
  auto BadProfile = llvmScalarStateContractX64(
      static_cast<InterpreterMachineStateProfile>(255), {});
  EXPECT_FALSE(bool(BadProfile));
  llvm::consumeError(BadProfile.takeError());
}

class LLVMScalarStateCompiled : public NeverDLiftTest {};
TEST_F(LLVMScalarStateCompiled, OriginalAndAggregateBridgeMatchAtO0AndO2) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  Source S(Loop);
  auto R =
      projectState(S, {8, 32, {{0, 7, 0}}, {{4, 32, false}, {0, 32, true}}});
  ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
  auto Original = tmpFile("original.ll"), Projected = tmpFile("scalar.ll"),
       Harness = tmpFile("check.c");
  S.function().setName("original");
  std::ofstream(Original) << S.text();
  std::string ScalarText;
  llvm::raw_string_ostream OS(ScalarText);
  R.Module->print(OS, nullptr);
  // An LLVM bridge avoids assuming that a native C struct has LLVM's ABI.
  std::ofstream(Projected) << ScalarText << R"(
define void @bridge(i32 %x, i32 %y, ptr %out) {
 %s = call {i64, i32, i32} @f(i32 %x, i32 %y)
 %a = extractvalue {i64, i32, i32} %s, 0
 %b = extractvalue {i64, i32, i32} %s, 1
 %c = extractvalue {i64, i32, i32} %s, 2
 %bw = zext i32 %b to i64
 %cw = zext i32 %c to i64
 %p = getelementptr i64, ptr %out, i64 1
 %q = getelementptr i64, ptr %out, i64 2
 store i64 %a, ptr %out
 store i64 %bw, ptr %p
 store i64 %cw, ptr %q
 ret void
})";
  std::ofstream(Harness) << R"(
#include <stdint.h>
#include <string.h>
uint64_t original(void *);
void bridge(uint32_t, uint32_t, uint64_t *);
int main(void) {
 uint64_t seed = UINT64_C(0x39fa102d49c861e7);
 for (unsigned k = 0; k < 8192; ++k) {
   seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
   uint32_t x = (uint32_t)seed, y = (uint32_t)(seed >> 32);
   uint64_t state = ((uint64_t)y << 32) | (x & 7);
   uint64_t out[3], status = original(&state);
   bridge(x, y, out);
   if (out[0] != status || out[1] != (uint32_t)(state >> 32) ||
       out[2] != ((uint32_t)state ^ (x & 7)) ||
       out[1] != (uint32_t)(y + 5 * (x & 3))) return 1;
 }
 return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    auto Program = tmpFile("oracle");
    auto Built =
        exec(NEVERD_TEST_CLANG,
             {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
              Original.string(), Projected.string(), Harness.string(), "-o",
              Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}
} // namespace neverd::analysis::scalar_test

namespace neverd::analysis::scalar_test {
TEST_F(LLVMScalarStateCompiled, MixedWidthsAndUnalignedWindowsMatchByteOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  auto IR = tmpFile("mixed.ll"), Harness = tmpFile("mixed.c");
  std::ofstream Out(IR), C(Harness);
  Out << Layout;
  C << R"(
#include <stdint.h>
#include <string.h>
static uint64_t word(const void *p, unsigned bytes) {
 uint64_t v = 0; memcpy(&v, p, bytes); return v;
}
)";
  std::vector<std::pair<unsigned, unsigned>> Cases;
  for (unsigned Cell : {8U, 16U, 32U, 64U})
    for (unsigned Bits : {8U, 16U, 32U, 64U})
      for (unsigned Offset : {0U, 1U, 5U, 8U, 12U}) {
        if (Offset + Bits / 8 > 16)
          continue;
        const auto N = Cases.size();
        Cases.emplace_back(Offset, Bits / 8);
        const auto Ty = "i" + std::to_string(Bits);
        std::string Body = std::string(Layout) +
                           "define i64 @f(ptr noundef %s) {\n"
                           "%p = getelementptr i8, ptr %s, i64 8\n"
                           "%v = load " +
                           Ty +
                           ", ptr %p, align 1\n"
                           "%q = getelementptr i8, ptr %s, i64 " +
                           std::to_string(Offset) + "\nstore " + Ty +
                           " %v, ptr %q, align 1\nret i64 0}\n";
        Source S(Body);
        auto R = projectState(S, {16,
                                  Cell,
                                  {},
                                  {{0, 64, false},
                                   {1, 64, false},
                                   {8, 64, false},
                                   {Offset, Bits, true}}});
        ASSERT_EQ(R.Status, StateProjection::Projected) << R.Diagnostic;
        auto *OriginalFunction = &S.function();
        OriginalFunction->setName("original" + std::to_string(N));
        auto *F = R.Module->getFunction("f");
        F->setName("scalar" + std::to_string(N));
        std::string Text;
        llvm::raw_string_ostream OS(Text);
        OriginalFunction->print(OS);
        F->print(OS);
        Out << Text;
        const auto Return = "{i64,i64,i64,i64," + Ty + "}";
        Out << "\ndefine void @bridge" << N << "(ptr %in, ptr %out) {\n";
        const auto InputTy = "i" + std::to_string(Cell);
        for (unsigned I = 0; I < R.Arguments; ++I)
          Out << "%p" << I << " = getelementptr i8, ptr %in, i64 "
              << I * (Cell / 8) << "\n%a" << I << " = load " << InputTy
              << ", ptr %p" << I << ", align 1\n";
        Out << "%s = call " << Return << " @" << F->getName().str() << "(";
        for (unsigned I = 0; I < R.Arguments; ++I)
          Out << (I ? "," : "") << InputTy << " %a" << I;
        Out << ")\n";
        for (unsigned I = 0; I < 5; ++I) {
          Out << "%r" << I << " = extractvalue " << Return << " %s, " << I
              << "\n";
          if (I == 4 && Bits != 64)
            Out << "%wide = zext " << Ty << " %r4 to i64\n";
          Out << "%o" << I << " = getelementptr i64, ptr %out, i64 " << I
              << "\nstore i64 "
              << (I == 4 && Bits != 64 ? "%wide" : "%r" + std::to_string(I))
              << ", ptr %o" << I << "\n";
        }
        Out << "ret void\n}\n";
        C << "uint64_t original" << N << "(void *);\nvoid bridge" << N
          << "(void *, uint64_t *);\n";
      }
  C << "int main(void) {\nuint64_t seed=UINT64_C(0xb453104ba1cddc73);\n"
       "for(unsigned k=0;k<1024;++k) {\n"
       "uint64_t input[2];\nfor(unsigned j=0;j<2;++j) {\n"
       "seed^=seed<<13; seed^=seed>>7; seed^=seed<<17;\n"
       "input[j]=k<4 ? "
       "(k==0?0:k==1?UINT64_MAX:k==2?UINT64_C(0x8000000000000000):1):seed;\n}"
       "\n";
  for (unsigned N = 0; N < Cases.size(); ++N) {
    auto [Offset, Bytes] = Cases[N];
    C << "{ uint64_t state[2],out[5]; memcpy(state,input,16);\n"
      << "uint64_t status=original" << N << "(state); bridge" << N
      << "(input,out);\nif(out[0]!=status || out[1]!=word(state,8) || "
         "out[2]!=word((char*)state+1,8) || out[3]!=word((char*)state+8,8) || "
         "out[4]!=(word((char*)state+"
      << Offset << "," << Bytes << ")^word((char*)input+" << Offset << ","
      << Bytes << "))) return " << N + 1 << "; }\n";
  }
  C << "}return 0;}\n";
  Out.close();
  C.close();
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    auto Program = tmpFile("mixed-oracle");
    auto Built =
        exec(NEVERD_TEST_CLANG,
             {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
              IR.string(), Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << "case " << Ran.exitCode << ": " << Ran.err;
  }
}
} // namespace neverd::analysis::scalar_test
