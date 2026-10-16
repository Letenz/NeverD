//===- MedCallLeftoversTests.cpp - Values calls leave in registers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedIR.h"

#include <utility>
#include <vector>

namespace {

using namespace neverd;

MedVar value(int Id, int Version, uint64_t Offset, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = Size;
  V.RegOff = Offset;
  V.TheArch = Arch::AArch64;
  return V;
}

PhiNode join(MedVar Output, std::vector<std::pair<int, MedVar>> Args) {
  PhiNode Phi;
  Phi.Output = Output;
  Phi.Args = std::move(Args);
  return Phi;
}

} // namespace

// What a register holds after calls change it, and every join of nothing
// else, loops of joins included, is left by calls; a join that anything else
// reaches is not, nor is a clobber that keeps part of its value or may be a
// result field.
TEST(MedCallLeftovers, JoinsOfCallClobbersOnly) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const uint64_t X1 = TRI.IntParamRegs[1], X2 = TRI.IntParamRegs[2],
                 X3 = TRI.IntParamRegs[3], V0 = TRI.FPParamRegs[0];
  MedFunc F;
  F.Blocks.resize(5);
  for (int I = 0; I < 5; ++I)
    F.Blocks[I].Id = I;
  // Calls in blocks 1 and 2 clobber x1-x3, and one keeps a vector
  // register's low half; block 3 joins them, and block 4 loops.
  const MedVar X2A = value(2, 1, X2), X2B = value(2, 2, X2);
  const MedVar X3A = value(3, 1, X3), X3Entry = value(3, 0, X3);
  const MedVar X1A = value(1, 1, X1), X1B = value(1, 2, X1);
  const MedVar Kept = value(8, 1, V0, 16);
  F.CallClobbers = {{X2A, 1}, {X2B, 2}, {X3A, 1},
                    {X1A, 1}, {X1B, 2}, {Kept, 1, value(8, 0, V0, 16), 8}};
  F.StructReturnCandidates = {{1, {value(0, 1, TRI.IntParamRegs[0]), X1A}}};
  const MedVar X2Join = value(2, 3, X2), X2Loop = value(2, 4, X2);
  const MedVar X3Join = value(3, 2, X3), X1Join = value(1, 3, X1);
  const MedVar X1Partial = value(1, 4, X1);
  F.Blocks[3].Phis = {join(X2Join, {{1, X2A}, {2, X2B}}),
                      join(X3Join, {{1, X3A}, {2, X3Entry}}),
                      join(X1Join, {{1, X1B}, {2, X1B}}),
                      join(X1Partial, {{1, X1A}, {2, X1B}})};
  F.Blocks[4].Phis = {join(X2Loop, {{3, X2Join}, {4, X2Loop}})};

  const ValuesLeftByCalls Left(F);
  EXPECT_TRUE(Left.contains(X2A));
  EXPECT_TRUE(Left.contains(X2Join));
  EXPECT_TRUE(Left.contains(X2Loop));
  EXPECT_TRUE(Left.contains(X1Join));
  // Version zero is what x3 brought in: the join keeps a value.
  EXPECT_FALSE(Left.contains(X3Join));
  EXPECT_FALSE(Left.contains(X3Entry));
  // Call 1 may return a field in x1, which the join then carries.
  EXPECT_FALSE(Left.contains(X1A));
  EXPECT_FALSE(Left.contains(X1Partial));
  // The call keeps the register's low half.
  EXPECT_FALSE(Left.contains(Kept));

  // The exception dispatcher entering the loop brings another value.
  F.Blocks[4].Phis[0].ExceptionalEntry = X3Entry;
  EXPECT_FALSE(ValuesLeftByCalls(F).contains(X2Loop));
  EXPECT_TRUE(ValuesLeftByCalls(F).contains(X2Join));
}
