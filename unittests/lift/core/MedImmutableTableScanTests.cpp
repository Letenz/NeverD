//===- MedImmutableTableScanTests.cpp - Immutable table scan semantics ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

using namespace neverd;
namespace {
MedVar var(int Id, uint16_t Width) {
  MedVar V;
  V.Id = Id;
  V.Size = Width;
  V.SSAVer = 1;
  return V;
}
MedOp op(NdOp Code, MedVar Out, std::initializer_list<MedVar> Inputs) {
  MedOp O;
  O.Opcode = Code;
  O.Output = Out;
  for (const auto &V : Inputs)
    O.addInput(V);
  return O;
}
BinaryImage table(unsigned Width, unsigned Count, bool Terminated = true) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Width == 8 ? Arch::X64 : Arch::X86;
  Image.Bits = Width == 8 ? Bitness::Bits64 : Bitness::Bits32;
  Segment S;
  S.Name = ".rdata";
  S.VA = 0x5000;
  S.Size = S.FileSz = (Count + 2) * Width;
  S.Flags = SegmentFlags::Readable;
  S.Data.assign(S.Size, 0);
  for (unsigned I = 0; I < Width; ++I)
    S.Data[I] = 0xff;
  for (unsigned I = 1; I <= Count + !Terminated; ++I)
    S.Data[I * Width] = 42;
  Image.Segments.push_back(S);
  Section Section;
  Section.Name = S.Name;
  Section.VA = S.VA;
  Section.Size = S.Size;
  Section.FileSz = S.FileSz;
  Section.Flags = S.Flags;
  Image.Sections.push_back(Section);
  return Image;
}
MedFunc scan(unsigned Width) {
  MedFunc F;
  F.Entry = 0x1000;
  F.Blocks.resize(3);
  for (unsigned I = 0; I < 3; ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 16;
    F.Blocks[I].EndAddr = 0x1010 + I * 16;
  }
  F.Blocks[0].Succs = {1};
  auto &Loop = F.Blocks[1];
  Loop.Preds = {0, 1};
  Loop.Succs = {2, 1};
  Loop.Phis.push_back(
      {var(1, 4),
       {{0, MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar)},
        {1, var(2, 4)}}});
  Loop.Ops = {
      op(NdOp::INT_ADD, var(2, 4), {var(1, 4), MedVar::makeConst(1, 4)}),
      op(NdOp::INT_ZEXT, var(3, Width), {var(2, 4)}),
      op(NdOp::INT_MULT, var(4, Width),
         {var(3, Width), MedVar::makeConst(Width, Width)}),
      op(NdOp::INT_ADD, var(5, Width),
         {MedVar::makeConst(0x5000, Width,
                            ConstantAddressProvenance::DataAddress, 0x5000),
          var(4, Width)}),
      op(NdOp::LOAD, var(6, Width), {var(5, Width)}),
      op(NdOp::INT_NOTEQUAL, var(7, 1),
         {var(6, Width), MedVar::makeConst(0, Width)}),
      op(NdOp::COND_BR, {}, {MedVar::makeConst(0x1010, Width), var(7, 1)})};
  F.Blocks[2].Preds = {1};
  F.Blocks[2].Ops = {op(NdOp::RETURN, {}, {var(1, 4)})};
  return F;
}

TEST(MedImmutableTableScans, EvaluatesSentinelCountAtItsExactExit) {
  for (unsigned Width : {4u, 8u})
    for (unsigned Count : {0u, 1u, 4u, 128u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Count);
      auto Image = table(Width, Count);
      auto F = scan(Width);
      ASSERT_TRUE(foldImmutableTableScans(F, Image));
      EXPECT_TRUE(F.Blocks[1].Phis.empty());
      EXPECT_EQ(F.Blocks[1].Succs, std::vector<int>{2});
      ASSERT_FALSE(F.Blocks[1].Ops.empty());
      const auto &CountCopy = F.Blocks[1].Ops.front();
      EXPECT_EQ(CountCopy.Output, var(1, 4));
      EXPECT_EQ(CountCopy.Inputs[0].ConstVal, Count);
      EXPECT_EQ(CountCopy.Inputs[0].Provenance,
                ConstantAddressProvenance::Scalar);
      EXPECT_FALSE(foldImmutableTableScans(F, Image));
    }
}

TEST(MedImmutableTableScans,
     RefusesMutableIncompleteAndIndependentlyEnteredScans) {
  for (unsigned Case = 0; Case < 11; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = table(8, Case == 6 ? 4097 : 4, Case != 0);
    auto F = scan(8);
    if (Case == 1)
      Image.Segments[0].Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    if (Case == 2)
      Image.Sections[0].FileSz = 8;
    if (Case == 3)
      F.ModuleAnalysisRoots.insert(0x1010);
    if (Case == 4)
      Image.CodeRefTargets.insert(0x1010);
    if (Case == 5)
      F.Blocks[1].Ops[4].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Case == 7)
      F.Blocks[1].Phis[0].Args[0].second = var(100, 4);
    if (Case == 8)
      F.ModuleAnalysisRoots.insert(0x1014);
    if (Case == 9)
      Image.CodeRefTargets.insert(0x1014);
    if (Case == 10)
      F.Entry = 0x1010;
    foldImmutableTableScans(F, Image);
    EXPECT_EQ(F.Blocks[1].Phis.size(), 1u);
    EXPECT_EQ(F.Blocks[1].Succs, (std::vector<int>{2, 1}));
  }
}

TEST(MedImmutableTableScans, ScalarFoldingKeepsTheOperationWidth) {
  auto Image = table(8, 1);
  MedFunc F;
  F.Blocks.resize(1);
  auto C = [](uint64_t V, unsigned Width) {
    return MedVar::makeConst(V, Width, ConstantAddressProvenance::Scalar);
  };
  F.Blocks[0].Ops = {op(NdOp::INT_ADD, var(1, 8), {C(255, 1), C(1, 1)}),
                     op(NdOp::INT_SUB, var(2, 8), {C(0, 1), C(1, 1)}),
                     op(NdOp::INT_MULT, var(3, 8), {C(255, 1), C(255, 1)}),
                     op(NdOp::INT_SLESS, var(4, 1), {C(255, 1), C(1, 8)}),
                     op(NdOp::INT_SLESS, var(5, 1), {C(255, 1), C(1, 1)}),
                     op(NdOp::INT_LEFT, var(6, 8), {C(128, 1), C(1, 1)}),
                     op(NdOp::INT_LEFT, var(7, 8), {C(1, 1), C(8, 8)})};
  ASSERT_TRUE(foldImmutableTableScans(F, Image));
  const uint64_t Expected[] = {0, 255, 1, 0, 1, 0, 256};
  for (unsigned I = 0; I < std::size(Expected); ++I) {
    SCOPED_TRACE(I);
    EXPECT_EQ(F.Blocks[0].Ops[I].Opcode, NdOp::COPY);
    EXPECT_EQ(F.Blocks[0].Ops[I].Inputs[0].ConstVal, Expected[I]);
  }
}

TEST(MedImmutableTableScans, RetainsRelocatableArithmeticAndComparisons) {
  auto Image = table(8, 1);
  MedFunc F;
  F.Blocks.resize(1);
  auto Unknown = MedVar::makeConst(0x5000, 8);
  auto Scalar = MedVar::makeConst(0x5000, 8, ConstantAddressProvenance::Scalar);
  auto Address =
      MedVar::makeConst(0x5000, 8, ConstantAddressProvenance::DataAddress);
  auto OtherAddress = Address;
  OtherAddress.ConstVal += 8;
  F.Blocks[0].Ops = {
      op(NdOp::INT_EQUAL, var(1, 1), {Unknown, Scalar}),
      op(NdOp::INT_ADD, var(2, 8), {Unknown, MedVar::makeConst(8, 8)}),
      op(NdOp::INT_EQUAL, var(3, 1), {Address, OtherAddress})};
  EXPECT_FALSE(foldImmutableTableScans(F, Image));
  EXPECT_EQ(F.Blocks[0].Ops[0].Opcode, NdOp::INT_EQUAL);
  EXPECT_EQ(F.Blocks[0].Ops[1].Opcode, NdOp::INT_ADD);
  EXPECT_EQ(F.Blocks[0].Ops[2].Opcode, NdOp::INT_EQUAL);
}

TEST(MedImmutableTableScans, OnePastAddressKeepsItsOriginalOwner) {
  auto Image = table(8, 1);
  auto Next = Image.Segments[0];
  Next.VA += Next.Size;
  Image.Segments.push_back(Next);
  auto NextSection = Image.Sections[0];
  NextSection.VA = Next.VA;
  Image.Sections.push_back(NextSection);
  auto End = MedVar::makeConst(Next.VA, 8,
                               ConstantAddressProvenance::DataAddress, 0x5000);
  MedFunc F;
  F.Blocks.resize(1);
  F.Blocks[0].Ops = {
      op(NdOp::INT_SUB, var(1, 8), {End, MedVar::makeConst(8, 8)}),
      op(NdOp::INT_ADD, var(2, 8), {End, MedVar::makeConst(8, 8)}),
      op(NdOp::LOAD, var(3, 8), {End})};
  ASSERT_TRUE(foldImmutableTableScans(F, Image));
  const auto &Previous = F.Blocks[0].Ops[0];
  EXPECT_EQ(Previous.Opcode, NdOp::COPY);
  EXPECT_EQ(Previous.Inputs[0].ConstVal, Next.VA - 8);
  EXPECT_EQ(Previous.Inputs[0].AddressOwnerVA, 0x5000u);
  EXPECT_EQ(F.Blocks[0].Ops[1].Opcode, NdOp::INT_ADD);
  EXPECT_EQ(F.Blocks[0].Ops[2].Opcode, NdOp::LOAD);
}
} // namespace
