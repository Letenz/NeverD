//===- ModuleJumpTableBudgetTests.cpp - Bounded module inventory ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Separates module input capacity, retained evidence and repeated CFG work.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "PipelineLowIRDetail.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>
#include <vector>

using namespace neverd;

namespace {

class ModuleJumpTableBudget : public NeverDLiftTest {
protected:
  struct Inputs {
    BinaryImage Image;
    std::vector<LowFunc> Functions;
  };

  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  std::optional<Inputs> fixture(unsigned CodeReferences = 0) {
    const auto Source = tmpFile("inventory.s");
    const auto Object = tmpFile("inventory.o");
    {
      std::ifstream Original(fs::path(TEST_SOURCE_DIR) /
                             "x86_64/test_module_address_owner_induction.s");
      if (!Original)
        return std::nullopt;
      std::ofstream Out(Source);
      Out << Original.rdbuf();
      if (CodeReferences) {
        Out << "\n.section .text.module_inventory,\"ax\",@progbits\n"
               ".globl module_inventory\n"
               ".type module_inventory,@function\nmodule_inventory:\n";
        for (unsigned I = 0; I < CodeReferences; ++I)
          Out << "leaq module_owner_safe_dispatch(%rip), %rax\n";
        Out << "xorl %eax,%eax\nretq\n"
               ".size module_inventory,.-module_inventory\n";
      }
    }
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    if (!Built.ok())
      return std::nullopt;
    auto Image = loadBinary(Object);
    EXPECT_TRUE(static_cast<bool>(Image));
    if (!Image) {
      llvm::consumeError(Image.takeError());
      return std::nullopt;
    }
    Inputs Result{std::move(*Image), {}};
    std::vector<const char *> Names{"module_owner_safe_dispatch",
                                    "module_owner_mutated_dispatch",
                                    "module_owner_mutated_loop_writer",
                                    "module_owner_unrelated_pointer_induction"};
    if (CodeReferences)
      Names.push_back("module_inventory");
    std::set<va_t> Entries;
    for (const char *Name : Names) {
      const Symbol *S = Result.Image.findSymbol(Name);
      EXPECT_NE(S, nullptr) << Name;
      if (!S)
        return std::nullopt;
      Entries.insert(S->Addr);
    }
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Result.Image.Arch));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    for (const char *Name : Names) {
      const Symbol *S = Result.Image.findSymbol(Name);
      Result.Functions.push_back(
          Builder.build(Result.Image, Dec, S->Addr, Name));
    }
    return Result;
  }

  static size_t opCount(const std::vector<LowFunc> &Functions) {
    size_t Count = 0;
    for (const LowFunc &F : Functions)
      for (const LowBlock &B : F.Blocks)
        Count += B.Ops.size();
    return Count;
  }

  static std::set<va_t> requested(const std::vector<LowFunc> &Functions) {
    std::set<va_t> Slots;
    for (const LowFunc &F : Functions)
      for (const JumpTable &JT : F.JumpTables)
        Slots.insert(JT.SuppressibleRelocationSlots.begin(),
                     JT.SuppressibleRelocationSlots.end());
    return Slots;
  }

  static LowFunc ordinaryPadding(size_t Ops) {
    LowFunc F;
    F.Entry = 0x70000000;
    F.Name = "ordinary_padding";
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    B.EndAddr = F.Entry + Ops;
    // Hand-built LowIR exercises inventory capacity without manufacturing any
    // relocation, address, call-result or table certificate.
    B.Ops.resize(Ops);
    for (LowOp &Op : B.Ops)
      Op.Opcode = NdOp::NOP;
    F.Blocks.push_back(std::move(B));
    return F;
  }

  static void expectAbandoned(const Inputs &I) {
    const auto Slots = requested(I.Functions);
    ASSERT_FALSE(Slots.empty());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I.Image, I.Functions);
    EXPECT_FALSE(Result.AnalysisComplete);
    EXPECT_EQ(Result.ProtectedRelocationSlots, Slots);
    for (const LowFunc &F : I.Functions)
      for (const JumpTable &JT : F.JumpTables)
        EXPECT_EQ(Result.UnsafeBranches.count(JT.InsnAddr), 1u);
  }
};

TEST_F(ModuleJumpTableBudget, InputCapacityDoesNotSpendRetainedEvidence) {
  auto I = fixture();
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto Baseline = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Baseline.AnalysisComplete);
  const auto SafeSlots =
      I->Functions.front().JumpTables.front().SuppressibleRelocationSlots;
  ASSERT_FALSE(SafeSlots.empty());
  for (va_t Slot : SafeSlots)
    ASSERT_EQ(Baseline.ProtectedRelocationSlots.count(Slot), 0u);
  const size_t BaseOps = opCount(I->Functions);
  for (size_t Total :
       {size_t{65537}, limits::kMaxSSAFunctionOps - 1,
        limits::kMaxSSAFunctionOps, limits::kMaxSSAFunctionOps + 1}) {
    SCOPED_TRACE(Total);
    I->Functions.push_back(ordinaryPadding(Total - BaseOps));
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Reverse);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      if (Total <= limits::kMaxSSAFunctionOps) {
        EXPECT_TRUE(Result.AnalysisComplete);
        EXPECT_EQ(Result.ProtectedRelocationSlots,
                  Baseline.ProtectedRelocationSlots);
        EXPECT_EQ(Result.UnsafeBranches, Baseline.UnsafeBranches);
      } else {
        expectAbandoned(*I);
      }
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
    }
    I->Functions.pop_back();
  }
}

TEST_F(ModuleJumpTableBudget, UnrelatedCodeOccurrencesUseTheScanAccount) {
  auto I = fixture(1100);
  ASSERT_TRUE(I);
  ASSERT_GE(I->Functions.back().RelocatedInstructionAddressOccurrences.size(),
            1100u);
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Result.AnalysisComplete);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto Slots =
      I->Functions.front().JumpTables.front().SuppressibleRelocationSlots;
  ASSERT_FALSE(Slots.empty());
  for (va_t Slot : Slots)
    EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
}

TEST_F(ModuleJumpTableBudget, SharedFieldKeepsEveryIndependentSource) {
  auto I = fixture();
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto &JT = I->Functions.front().JumpTables.front();
  const auto Slots = JT.SuppressibleRelocationSlots;
  ASSERT_FALSE(Slots.empty());
  LowFunc Independent = I->Functions.front();
  const auto SourceIt =
      std::find_if(Independent.RelocatedInstructionAddressOccurrences.begin(),
                   Independent.RelocatedInstructionAddressOccurrences.end(),
                   [&](const auto &Occurrence) {
                     return Occurrence.FieldVA != InvalidVA &&
                            Occurrence.TargetVA == JT.BaseAddr;
                   });
  ASSERT_NE(SourceIt, Independent.RelocatedInstructionAddressOccurrences.end());
  const auto Source = *SourceIt;
  const auto BlockIt =
      std::find_if(Independent.Blocks.begin(), Independent.Blocks.end(),
                   [&](const LowBlock &B) {
                     return B.StartAddr <= Source.InstructionAddr &&
                            Source.InstructionAddr < B.EndAddr;
                   });
  ASSERT_NE(BlockIt, Independent.Blocks.end());
  const auto BoundaryIt = std::find_if(
      BlockIt->InstructionBoundaries.begin(),
      BlockIt->InstructionBoundaries.end(), [&](const auto &Boundary) {
        return Boundary.Address == Source.InstructionAddr;
      });
  ASSERT_NE(BoundaryIt, BlockIt->InstructionBoundaries.end());
  LowBlock Prefix = *BlockIt;
  const size_t PrefixOps = BoundaryIt->FirstOp + BoundaryIt->OpCount;
  ASSERT_LE(PrefixOps, Prefix.Ops.size());
  Prefix.Ops.resize(PrefixOps);
  Prefix.InstructionBoundaries.resize(
      BoundaryIt - BlockIt->InstructionBoundaries.begin() + 1);
  Prefix.Preds.clear();
  Prefix.Succs.clear();
  Prefix.ExceptionalPreds.clear();
  Prefix.ExceptionalSuccs.clear();
  Prefix.EndAddr = BoundaryIt->Address + BoundaryIt->Size + 1;
  LowOp Overwrite;
  Overwrite.Opcode = NdOp::COPY;
  Overwrite.Addr = Prefix.EndAddr - 1;
  Overwrite.Output =
      NdVar::reg(getTargetRegInfo(I->Image.Arch).IntReturnReg, 8);
  Overwrite.addInput(NdVar::cst(0, 8));
  Prefix.Ops.push_back(Overwrite);
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Overwrite.Addr;
  Return.Seq = 1;
  Return.addInput(Overwrite.Output);
  Prefix.Ops.push_back(Return);
  for (const LowOp &Op : Prefix.Ops) {
    ASSERT_NE(Op.Opcode, NdOp::LOAD);
    ASSERT_NE(Op.Opcode, NdOp::STORE);
    ASSERT_NE(Op.Opcode, NdOp::CALL);
    ASSERT_NE(Op.Opcode, NdOp::INDIR_CALL);
  }
  // This context retains the exact materialization prefix, then overwrites
  // the pointer and returns a scalar. No ordinary use can independently veto
  // suppression; only the matching source inventory can protect the owner.
  Independent.Name = "independent_context_without_table_witness";
  Independent.JumpTables.clear();
  Independent.Blocks = {std::move(Prefix)};
  Independent.ModuleAnalysisRoots = {Independent.Entry};
  Independent.OrdinaryModuleAnalysisRoots = {Independent.Entry};
  Independent.RelocatedInstructionAddressOccurrences.clear();
  I->Functions.push_back(std::move(Independent));
  const auto WithoutSource =
      pipeline_detail::arbitrateModuleJumpTablesForTesting(I->Image,
                                                           I->Functions);
  ASSERT_TRUE(WithoutSource.AnalysisComplete);
  for (va_t Slot : Slots)
    ASSERT_EQ(WithoutSource.ProtectedRelocationSlots.count(Slot), 0u);
  I->Functions.back().RelocatedInstructionAddressOccurrences = {Source};
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 1u);
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
  }
}

TEST_F(ModuleJumpTableBudget, MissingPCRelativeSourcesProtectTheirContainer) {
  auto I = fixture();
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto &JT = I->Functions.front().JumpTables.front();
  const auto SourceIt = std::find_if(
      I->Functions.front().RelocatedInstructionAddressOccurrences.begin(),
      I->Functions.front().RelocatedInstructionAddressOccurrences.end(),
      [&](const auto &Source) { return Source.TargetVA == JT.BaseAddr; });
  ASSERT_NE(SourceIt,
            I->Functions.front().RelocatedInstructionAddressOccurrences.end());
  const va_t FieldVA = SourceIt->FieldVA;
  ASSERT_NE(FieldVA, InvalidVA);
  ASSERT_NE(I->Image.DataAddressRelocOperands.count(FieldVA), 0u);
  ASSERT_TRUE(I->Image.DataAddressRelocOperands.at(FieldVA)
                  .PCRelativeFromInstructionEnd);
  for (LowFunc &F : I->Functions)
    std::erase_if(
        F.RelocatedInstructionAddressOccurrences,
        [&](const auto &Source) { return Source.FieldVA == FieldVA; });
  const auto Slots = requested(I->Functions);
  ASSERT_FALSE(Slots.empty());
  for (bool Reverse : {false, true}) {
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    EXPECT_EQ(Result.ProtectedRelocationSlots, Slots);
  }
}

TEST_F(ModuleJumpTableBudget,
       ScanAndSourceRetentionExhaustionProtectEverySlot) {
  for (unsigned Count : {5000u, 16400u}) {
    SCOPED_TRACE(Count);
    auto I = fixture(1);
    ASSERT_TRUE(I);
    auto &Sources = I->Functions.back().RelocatedInstructionAddressOccurrences;
    ASSERT_FALSE(Sources.empty());
    const auto Source = Sources.front();
    ASSERT_EQ(Source.Provenance, ConstantAddressProvenance::CodeAddress);
    Sources.assign(Count, Source);
    expectAbandoned(*I);
    std::reverse(I->Functions.begin(), I->Functions.end());
    expectAbandoned(*I);
  }
}

TEST_F(ModuleJumpTableBudget, QueueAndRepeatedLongBlockWorkRemainBounded) {
  auto I = fixture();
  ASSERT_TRUE(I);
  LowFunc Chain = ordinaryPadding(1);
  Chain.Blocks.clear();
  constexpr size_t QueueLimit =
      size_t{limits::kMaxJumpTableEvidenceWork} * limits::kMaxMultiStageRetries;
  for (size_t Index = 0; Index < QueueLimit / 2 + 1; ++Index) {
    LowBlock B;
    B.Id = static_cast<int>(Index);
    B.StartAddr = Chain.Entry + Index;
    B.EndAddr = B.StartAddr + 1;
    B.Ops.resize(1);
    B.Ops.front().Opcode = NdOp::NOP;
    if (Index)
      B.Preds.push_back(B.Id - 1);
    B.Succs.push_back(B.Id + 1);
    Chain.Blocks.push_back(std::move(B));
  }
  Chain.Blocks.back().Succs.clear();
  I->Functions.push_back(std::move(Chain));
  expectAbandoned(*I);
  I->Functions.pop_back();

  LowFunc Loop = ordinaryPadding(100000);
  LowBlock &B = Loop.Blocks.front();
  B.Preds = {0};
  B.Succs = {0};
  LowOp Advance;
  Advance.Opcode = NdOp::INT_ADD;
  Advance.Output = NdVar::reg(getTargetRegInfo(I->Image.Arch).StackPointer, 8);
  Advance.addInput(Advance.Output);
  Advance.addInput(NdVar::cst(1, 8));
  B.Ops.push_back(Advance);
  I->Functions.push_back(std::move(Loop));
  expectAbandoned(*I);
}

TEST_F(ModuleJumpTableBudget, FinalRecordingSpendsTheSameTransferAccount) {
  auto I = fixture();
  ASSERT_TRUE(I);
  // Entry SP reaches a fixed point after three visits. A fourth transfer is
  // needed to record uses; only that last pass exhausts the larger input.
  for (size_t Ops : {size_t{220000}, size_t{300000}}) {
    SCOPED_TRACE(Ops);
    LowFunc Loop = ordinaryPadding(Ops);
    Loop.Blocks.front().Preds = {0};
    Loop.Blocks.front().Succs = {0};
    I->Functions.push_back(std::move(Loop));
    if (Ops == 220000) {
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      EXPECT_TRUE(Result.AnalysisComplete);
    } else {
      expectAbandoned(*I);
    }
    I->Functions.pop_back();
  }
}

TEST_F(ModuleJumpTableBudget, FunctionsShareTheTransferAccount) {
  auto I = fixture();
  ASSERT_TRUE(I);
  std::vector<LowFunc> Loops;
  for (va_t Entry : {va_t{0x70000000}, va_t{0x71000000}}) {
    LowFunc Loop = ordinaryPadding(150000);
    Loop.Entry = Entry;
    Loop.Blocks.front().StartAddr = Entry;
    Loop.Blocks.front().EndAddr = Entry + 150000;
    Loop.Blocks.front().Preds = {0};
    Loop.Blocks.front().Succs = {0};
    I->Functions.push_back(Loop);
    const auto Alone = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    EXPECT_TRUE(Alone.AnalysisComplete);
    I->Functions.pop_back();
    Loops.push_back(std::move(Loop));
  }
  for (LowFunc &Loop : Loops)
    I->Functions.push_back(std::move(Loop));
  ASSERT_LE(opCount(I->Functions), limits::kMaxSSAFunctionOps);
  expectAbandoned(*I);
  std::reverse(I->Functions.begin(), I->Functions.end());
  expectAbandoned(*I);
}

} // namespace
