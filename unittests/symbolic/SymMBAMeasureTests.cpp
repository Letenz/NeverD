//===- SymMBAMeasureTests.cpp - Corner measurement conventions
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace neverd::symbolic;

TEST(SymMBAMeasure, PreservesBinaryCornerOrderForSparsePermutedInputs) {
  for (unsigned Width : {1u, 3u, 8u, 64u, 65u, 129u, 4096u}) {
    for (unsigned Count : {1u, 2u, 5u, 8u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Count);
      SymContext Ctx;
      llvm::SmallVector<SymRef, 8> Vars;
      llvm::SmallVector<uint32_t, 8> Atoms;
      for (unsigned I = 0; I < Count; ++I) {
        Ctx.mkVar("unused" + std::to_string(I), Width);
        SymRef V = Ctx.mkVar("v" + std::to_string(I), Width);
        Vars.push_back(V);
        Atoms.push_back(Ctx.varId(V));
      }
      std::reverse(Atoms.begin(), Atoms.end());
      llvm::APInt Scale(Width, 3);
      if (Width > 64)
        Scale.setBit(Width - 2);
      SymRef E = Ctx.mkAdd({Ctx.mkConst(Width, 37),
                            Ctx.mkMul(Ctx.mkConst(Scale), Vars.front()),
                            Ctx.mkNot(Ctx.mkXor(Vars)), Ctx.mkAnd(Vars),
                            Ctx.mkMul(Vars.front(), Vars.back())});
      auto Weights = detail::measure(Ctx, E, Atoms);
      ASSERT_EQ(Weights.size(), size_t(1) << Count);

      const llvm::APInt Zero(Width, 0);
      const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
      std::vector<llvm::APInt> Assignment(Ctx.numVars(), Ones);
      for (size_t Pattern = 0; Pattern < Weights.size(); ++Pattern) {
        for (unsigned I = 0; I < Count; ++I)
          Assignment[Atoms[I]] = Pattern & (size_t(1) << I) ? Ones : Zero;
        EXPECT_EQ(Weights[Pattern], -Ctx.eval(E, Assignment)) << Pattern;
      }
    }
  }
}

TEST(SymMBAMeasure, ZeroInputsHaveOneNegatedConstantCorner) {
  for (unsigned Width : {1u, 8u, 64u, 65u, 257u}) {
    SymContext Ctx;
    Ctx.mkVar("unused", Width);
    for (const llvm::APInt &Value :
         {llvm::APInt(Width, 0), llvm::APInt(Width, 1),
          llvm::APInt::getAllOnes(Width)}) {
      auto Weights = detail::measure(Ctx, Ctx.mkConst(Value), {});
      ASSERT_EQ(Weights.size(), 1u);
      EXPECT_EQ(Weights[0], -Value);
    }
  }
}
