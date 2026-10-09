//===- ReturnContractTests.cpp - Functions that return no value ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// settleReturnContracts decides, on MedIR and for both C backends, which
/// functions return no value a caller could rely on.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineReturnModelingDetail.h"
#include "gtest/gtest.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include <vector>

using namespace neverd;

namespace {

constexpr va_t CalleeEntry = 0x1000;
constexpr va_t CallerEntry = 0x2000;

MedVar rax(int Version) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X64;
  Value.Id = 0;
  Value.SSAVer = Version;
  Value.Size = 8;
  Value.RegOff = x86reg::RAX;
  return Value;
}

MedVar temporary(int Id) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = Arch::X64;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.Size = 8;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output, std::vector<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const MedVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

/// The entry seed of RAX: the register as the function entered it.
MedOp seed() { return operation(NdOp::COPY, rax(0), {rax(0)}); }

MedOp returning(const MedVar &Value) {
  return operation(NdOp::RETURN, {}, {Value});
}

MedFunc function(va_t Entry, std::vector<MedBlock> Blocks) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "f" + std::to_string(Entry);
  Func.ReturnType = NdType::makeInt(8);
  Func.Blocks = std::move(Blocks);
  for (size_t I = 0; I < Func.Blocks.size(); ++I)
    Func.Blocks[I].Id = static_cast<int>(I);
  return Func;
}

/// A block that returns \p Value after \p Ops.
MedBlock block(std::vector<MedOp> Ops, std::vector<int> Preds = {},
               std::vector<int> Succs = {}) {
  MedBlock Block;
  Block.Ops = std::move(Ops);
  Block.Preds = std::move(Preds);
  Block.Succs = std::move(Succs);
  return Block;
}

/// One path hands back RAX as the function entered it; the other, what
/// \p Last defines.
MedFunc branching(va_t Entry, MedOp Last) {
  const MedVar Value = Last.Output;
  return function(Entry,
                  {block({seed(), operation(NdOp::COND_BR, {}, {temporary(9)})},
                         {}, {1, 2}),
                   block({returning(rax(0))}, {0}),
                   block({std::move(Last), returning(Value)}, {0})});
}

MedOp callTo(va_t Target, int Version) {
  return operation(
      NdOp::CALL, rax(Version),
      {MedVar::makeConst(Target, 8, ConstantAddressProvenance::CodeAddress)});
}

std::vector<bool> settle(std::vector<MedFunc> Funcs) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  PipelineResult Result;
  Result.MedFuncs = std::move(Funcs);
  settleReturnContracts(Image, Result);
  std::vector<bool> NoValue;
  for (const MedFunc &Func : Result.MedFuncs)
    NoValue.push_back(Func.ReturnsNoValue);
  return NoValue;
}

TEST(ReturnContracts, TheRegisterAsTheFunctionEnteredItIsNoValue) {
  EXPECT_EQ(
      settle({function(CalleeEntry, {block({seed(), returning(rax(0))})})}),
      std::vector<bool>{true});
}

TEST(ReturnContracts, TheResultOfACalleeThatReturnsNoneIsNoValue) {
  // tail_void: `jmp sink`, where sink hands back RAX as it entered.
  const MedFunc Sink =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  const MedFunc TailVoid = function(
      CallerEntry, {block({callTo(CalleeEntry, 1), returning(rax(1))})});
  EXPECT_EQ(settle({TailVoid, Sink}), (std::vector<bool>{true, true}));
}

TEST(ReturnContracts, TheResultOfAnUnknownCalleeIsAValue) {
  // PLT0: what the dynamic linker's resolver returns, the function returns.
  EXPECT_EQ(settle({function(CallerEntry,
                             {block({operation(NdOp::INDIR_CALL, rax(1),
                                               {MedVar::makeConst(0x3FC8, 8)}),
                                     returning(rax(1))})})}),
            std::vector<bool>{false});
}

TEST(ReturnContracts, AnUndefinedPathWithoutADeliberateValueIsNoValue) {
  // One path returns RAX as the caller left it, so no caller can rely on a
  // result, whatever the other path's call leaves.
  EXPECT_EQ(settle({branching(CalleeEntry, operation(NdOp::INDIR_CALL, rax(1),
                                                     {temporary(5)}))}),
            std::vector<bool>{true});
}

TEST(ReturnContracts, ACallerThatReadsTheResultKeepsAValue) {
  MedFunc Callee = branching(
      CalleeEntry, operation(NdOp::INDIR_CALL, rax(1), {temporary(5)}));
  // The caller adds 1 to what the callee returns.
  MedFunc Caller = function(
      CallerEntry, {block({callTo(CalleeEntry, 1),
                           operation(NdOp::INT_ADD, rax(2),
                                     {rax(1), MedVar::makeConst(1, 8)}),
                           returning(rax(2))})});
  EXPECT_EQ(settle({std::move(Callee), std::move(Caller)}),
            (std::vector<bool>{false, false}));
}

TEST(ReturnContracts, AConstantIsADeliberateValue) {
  // register_tm_clones' `return 0` beside the bare path: a value.
  EXPECT_EQ(
      settle({branching(CalleeEntry, operation(NdOp::COPY, rax(1),
                                               {MedVar::makeConst(0, 8)}))}),
      std::vector<bool>{false});
}

TEST(ReturnContracts, AFunctionThatNeverReturnsReturnsNoValue) {
  MedOp Abort = callTo(0x3000, 1);
  Abort.DoesNotReturn = true;
  EXPECT_EQ(settle({function(CalleeEntry, {block({seed(), Abort})})}),
            std::vector<bool>{true});
}

TEST(ReturnContracts, ADeclaredReturnTypeStays) {
  MedFunc Declared =
      function(CalleeEntry, {block({seed(), returning(rax(0))})});
  Declared.SourceTypeHint.emplace();
  EXPECT_EQ(settle({std::move(Declared)}), std::vector<bool>{false});
}

} // namespace
