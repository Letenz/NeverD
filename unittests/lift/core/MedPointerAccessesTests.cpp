//===- MedPointerAccessesTests.cpp - What code does through a pointer -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/ir/med/MedPointerAccesses.h"

#include <initializer_list>

namespace {

using namespace neverd;
using Use = MedPointerAccess::Use;

enum : int { kRCX = 3, kRDX = 4, kR10 = 5, kR8 = 6 };
/// Their offsets in x86-64's register space.
constexpr uint64_t RCX = 8, RDX = 16, R8 = 64, R10 = 80;

MedVar reg(int Id, uint64_t RegOff, int Version, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = Size;
  V.RegOff = RegOff;
  return V;
}
MedVar rcx(int Version) { return reg(kRCX, RCX, Version); }
MedVar rdx(int Version) { return reg(kRDX, RDX, Version); }
MedVar r10(int Version) { return reg(kR10, R10, Version); }
MedVar r8b(int Version) { return reg(kR8, R8, Version, 1); }

MedVar temp(int Id, int Version, uint16_t Size = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = Size;
  return V;
}

MedVar none() {
  MedVar V;
  V.Id = -1;
  return V;
}

MedVar constant(uint64_t Value, uint16_t Size) {
  return MedVar::makeConst(Value, Size);
}

MedOp op(va_t Addr, NdOp Opcode, MedVar Output,
         std::initializer_list<MedVar> Inputs) {
  MedOp Result;
  Result.Addr = Addr;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const MedVar &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

/// KeInitializeApc's stores through its first argument in MedIR, a read
/// through its second, a list entry's address passed on, and a load from a
/// constant address.
MedFunc initializeApc() {
  MedFunc Func;
  Func.Entry = 0x1000;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {
      op(0x1000, NdOp::COPY, rcx(0), {rcx(0)}),
      op(0x1003, NdOp::STORE, none(), {rcx(0), constant(0x12, 1)}),
      op(0x1006, NdOp::INT_ADD, temp(5, 2), {rcx(0), constant(2, 8)}),
      op(0x1006, NdOp::STORE, none(), {temp(5, 2), constant(0x58, 1)}),
      op(0x100a, NdOp::COPY, r10(1), {rcx(0)}),
      op(0x100d, NdOp::INT_ADD, temp(5, 9), {r10(1), constant(0x28, 8)}),
      op(0x100d, NdOp::STORE, none(), {temp(5, 9), rdx(0)}),
      op(0x1011, NdOp::INT_ADD, temp(5, 7), {rcx(0), constant(0x50, 8)}),
      op(0x1011, NdOp::STORE, none(), {temp(5, 7), r8b(0)}),
      op(0x1015, NdOp::INT_ADD, temp(5, 34), {rdx(0), constant(0x24A, 8)}),
      op(0x1015, NdOp::LOAD, temp(12, 6, 1), {temp(5, 34)}),
      // lea rdx, [rcx+10h]; call InitializeListHead
      op(0x1019, NdOp::INT_ADD, temp(20, 1), {rcx(0), constant(0x10, 8)}),
      op(0x101d, NdOp::CALL, none(), {constant(0x2000, 8), temp(20, 1)}),
      // A pointer before the record: CONTAINING_RECORD's subtraction.
      op(0x1022, NdOp::INT_SUB, temp(21, 1), {rcx(0), constant(0x10, 8)}),
      op(0x1022, NdOp::LOAD, temp(22, 1, 1), {temp(21, 1)}),
      op(0x1026, NdOp::LOAD, temp(23, 1), {constant(0x3000, 8)}),
  };
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

} // namespace

TEST(MedPointerAccesses, GathersTheAccessesThroughAPointer) {
  const MedFunc Func = initializeApc();
  const MedPointerAccesses Accesses(Func);
  const std::vector<MedPointerAccess> Through = Accesses.through(rcx(0));
  // A copy of the pointer is the pointer.
  const std::vector<MedPointerAccess> Expected = {
      {0x1003, 0x00, 1, Use::Write},   {0x1006, 0x02, 1, Use::Write},
      {0x100d, 0x28, 8, Use::Write},   {0x1011, 0x50, 1, Use::Write},
      {0x1019, 0x10, 0, Use::Address}, {0x1022, -0x10, 1, Use::Read}};
  EXPECT_EQ(Through, Expected);
  EXPECT_EQ(Accesses.through(rdx(0)),
            (std::vector<MedPointerAccess>{{0x1015, 0x24A, 1, Use::Read}}));
}

TEST(MedPointerAccesses, FindsThePointerOfAnInstruction) {
  const MedFunc Func = initializeApc();
  const MedPointerAccesses Accesses(Func);
  const auto Store = Accesses.at(0x100d);
  ASSERT_TRUE(Store);
  EXPECT_EQ(Store->first, rcx(0));
  EXPECT_EQ(Store->second, (MedPointerAccess{0x100d, 0x28, 8, Use::Write}));
  const auto Lea = Accesses.at(0x1019);
  ASSERT_TRUE(Lea);
  EXPECT_EQ(Lea->second.Kind, Use::Address);
  EXPECT_EQ(Lea->second.Offset, 0x10);
  // A constant address is no pointer's, and a call reaches no memory.
  EXPECT_FALSE(Accesses.at(0x1026));
  EXPECT_FALSE(Accesses.at(0x101d));
  const auto Base = Accesses.baseOf(temp(5, 9));
  ASSERT_TRUE(Base);
  EXPECT_EQ(Base->first, rcx(0));
  EXPECT_EQ(Base->second, 0x28);
}
