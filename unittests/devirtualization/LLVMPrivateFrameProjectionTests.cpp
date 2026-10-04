//===- LLVMPrivateFrameProjectionTests.cpp - Initialized frame checks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMPrivateFrameProjectionTest.h"

#include "neverd/analysis/LLVMMemoryAnalysis.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"

namespace neverd::analysis::frame_test {
namespace {
using Result = LLVMPrivateFrameResult;

void refuses(const std::string &Text, llvm::StringRef Diagnostic) {
  llvm::LLVMContext C;
  auto M = parse(C, Text);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  const auto Before = print(*M);
  auto R = projectLLVMPrivateFrame(F, contract(F));
  EXPECT_EQ(R.Status, Result::Unsupported) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find(Diagnostic.str()), std::string::npos)
      << R.Diagnostic;
  EXPECT_EQ(print(*M), Before);
}
} // namespace

TEST(LLVMPrivateFrame, CoversOverlapsEveryEntryAndBothBackedges) {
  for (unsigned Width : {32u, 64u})
    for (bool BigEndian : {false, true}) {
      llvm::LLVMContext C;
      auto M = parse(C, program(Width), Width, BigEndian);
      ASSERT_TRUE(M);
      auto &F = *M->getFunction("f");
      auto R = projectLLVMPrivateFrame(F, contract(F));
      ASSERT_EQ(R.Status, Result::Projected) << R.Diagnostic;
      EXPECT_EQ(R.Begin, -48);
      EXPECT_EQ(R.End, -16);
      EXPECT_EQ(R.Loads, 3u);
      EXPECT_EQ(R.Stores, 6u);
      EXPECT_GT(R.Rounds, 1u);
      EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
      unsigned Memory = 0;
      for (auto &I : llvm::instructions(F)) {
        auto *L = llvm::dyn_cast<llvm::LoadInst>(&I);
        auto *S = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (L || S) {
          ++Memory;
          auto *Pointer = L ? L->getPointerOperand() : S->getPointerOperand();
          EXPECT_FALSE(llvm::isa<llvm::IntToPtrInst>(Pointer));
        }
      }
      EXPECT_EQ(Memory, 13u);
      // The original numeric frame value is still an external store operand.
      EXPECT_NE(print(*M).find("store i" + std::to_string(Width) + " %root"),
                std::string::npos);
    }
}

TEST(LLVMPrivateFrame, RejectsMissingEntryAndPartialWrites) {
  auto IR = program();
  replace(IR, "store i32 %flip, ptr %r, align 1", "");
  refuses(IR, "not definitely initialized");
  IR = program();
  replace(IR, "store i32 %plus, ptr %r, align 1",
          "%short = trunc i32 %plus to i16\n"
          "store i16 %short, ptr %r, align 1");
  refuses(IR, "not definitely initialized");
}

TEST(LLVMPrivateFrame, DerivesPositiveExtentsWithoutAStackConvention) {
  auto IR = program();
  replace(IR, "%first = sub i64 %root, 48", "%first = add i64 %root, 16");
  replace(IR, "%middle = add i64 -46, %root", "%middle = add i64 18, %root");
  replace(IR, "%last = add i64 %root, -20", "%last = add i64 %root, 44");
  llvm::LLVMContext C;
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  auto Contract = contract(F);
  Contract.Begin = 0;
  Contract.End = 64;
  auto R = projectLLVMPrivateFrame(F, Contract);
  EXPECT_EQ(R.Status, Result::Projected) << R.Diagnostic;
  EXPECT_EQ(R.Begin, 16);
  EXPECT_EQ(R.End, 48);
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
}

TEST(LLVMPrivateFrame, BackedgeStoresCannotInitializeTheFirstIteration) {
  auto IR = program();
  replace(IR, "store i32 %plus, ptr %r, align 1", "");
  replace(IR, "store i32 %flip, ptr %r, align 1", "");
  refuses(IR, "not definitely initialized");
}

TEST(LLVMPrivateFrame, RejectsUnknownAliasesAndEffectsWithoutMutation) {
  auto IR = program();
  replace(IR, "%last = add i64 %root, -20", "%last = xor i64 %root, 20");
  refuses(IR, "disjoint object contract");
  IR = program();
  replace(IR, "store i64 %seed", "store volatile i64 %seed");
  refuses(IR, "ordered");
  IR = program();
  replace(IR, "%packed = load i64, ptr %p, align 1",
          "%packed = load atomic i64, ptr %p monotonic, align 8");
  refuses(IR, "ordered");
  IR = "declare void @observe() memory(none) nounwind willreturn\n" + program();
  replace(IR, "done:", "done:\ncall void @observe()");
  refuses(IR, "encountered an effect");
}

TEST(LLVMPrivateFrame, KeepsPureIntrinsicsButRejectsExtraContracts) {
  const std::string Prefix = "declare i32 @llvm.ctpop.i32(i32)\n";
  auto IR = Prefix + program();
  replace(IR, "%flip = xor i32 %x, 39",
          "%flip = call i32 @llvm.ctpop.i32(i32 %x)");
  llvm::LLVMContext C;
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  auto R = projectLLVMPrivateFrame(F, contract(F));
  EXPECT_EQ(R.Status, Result::Projected) << R.Diagnostic;
  EXPECT_NE(print(*M).find("call i32 @llvm.ctpop.i32"), std::string::npos);
  replace(IR, "@llvm.ctpop.i32(i32 %x)",
          "@llvm.ctpop.i32(i32 %x) [ \"deopt\"(i32 %x) ]");
  refuses(IR, "encountered an effect");
}

TEST(LLVMPrivateFrame, RequiresCurrentObjectAndCompleteExtentContracts) {
  for (unsigned Variant = 0; Variant != 8; ++Variant) {
    llvm::LLVMContext C;
    auto M = parse(C, program());
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto Contract = contract(F);
    switch (Variant) {
    case 0:
      Contract.Begin = -47;
      break;
    case 1:
      Contract.End = -17;
      break;
    case 2:
      Contract.OtherObjects.clear();
      break;
    case 3:
      Contract.OtherObjects[0].Bytes = 31;
      break;
    case 4:
      Contract.OtherObjects[1].Bytes = 3;
      break;
    case 5:
      Contract.OtherObjects.push_back(Contract.OtherObjects[0]);
      break;
    case 6:
      Contract.Base = F.getArg(1);
      break;
    case 7:
      Contract.OtherObjects[0].Base = F.getArg(0);
      break;
    }
    const auto Before = print(*M);
    auto R = projectLLVMPrivateFrame(F, Contract);
    EXPECT_EQ(R.Status, Result::Unsupported) << R.Diagnostic;
    EXPECT_EQ(print(*M), Before);
  }
}

TEST(LLVMPrivateFrame, RejectsStackObservationsEvenWithNoMemoryEffects) {
  for (const char *Name : {"frameaddress", "returnaddress"}) {
    std::string Intrinsic = "@llvm." + std::string(Name) + ".p0";
    auto IR = "declare ptr " + Intrinsic + "(i32 immarg)\n" + program();
    replace(IR,
            "done:", "done:\n%observed = call ptr " + Intrinsic + "(i32 0)");
    replace(IR, "%answer = xor i64 %both, %root",
            "%observedBits = ptrtoint ptr %observed to i64\n"
            "%answer = xor i64 %both, %observedBits");
    llvm::LLVMContext C;
    auto M = parse(C, IR);
    ASSERT_TRUE(M);
    for (auto &I : llvm::instructions(M->getFunction("f")))
      if (llvm::isa<llvm::CallBase>(I))
        EXPECT_TRUE(isLLVMMemoryTransparentIntrinsic(I));
    refuses(IR, "encountered an effect");
  }
}

TEST(LLVMPrivateFrame, AcceptsAnExplicitEntryLoadWithoutNamingItsLocation) {
  auto IR = program();
  replace(IR, "entry:", "entry:\n%loaded = load i64, ptr %out, align 1");
  replace(IR, "%first = sub i64 %root", "%first = sub i64 %loaded");
  replace(IR, "%middle = add i64 -46, %root", "%middle = add i64 -46, %loaded");
  replace(IR, "%last = add i64 %root", "%last = add i64 %loaded");
  llvm::LLVMContext C;
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  auto Contract = contract(F);
  Contract.Base = &F.getEntryBlock().front();
  auto R = projectLLVMPrivateFrame(F, Contract);
  EXPECT_EQ(R.Status, Result::Projected) << R.Diagnostic;
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
}

TEST(LLVMPrivateFrame, RejectsTruncatedAddressesAndIncompatibleLayouts) {
  auto IR = program();
  replace(IR, "%p = inttoptr i64 %first to ptr",
          "%narrow = trunc i64 %first to i32\n"
          "%p = inttoptr i32 %narrow to ptr");
  refuses(IR, "unresolved");
  for (const char *Layout : {"e-p:64:64:64:32", "e-p:64:64-A1"}) {
    llvm::LLVMContext C;
    auto M = parse(C, program());
    ASSERT_TRUE(M);
    M->setDataLayout(Layout);
    auto &F = *M->getFunction("f");
    const auto Before = print(*M);
    auto R = projectLLVMPrivateFrame(F, contract(F));
    EXPECT_EQ(R.Status, Result::Unsupported);
    EXPECT_EQ(print(*M), Before);
  }
}

TEST(LLVMPrivateFrame, RefusesDynamicGEPMetadataAndUnreachableBlocks) {
  auto IR = program();
  replace(IR, "%out12 = getelementptr i8, ptr %out, i64 12",
          "%out12 = getelementptr i8, ptr %out, i32 %n");
  refuses(IR, "unresolved");
  IR = program();
  replace(IR, "%value = load i32, ptr %r, align 1",
          "%value = load i32, ptr %r, align 1, !range !0");
  IR += "\n!0 = !{i32 0, i32 64}\n";
  refuses(IR, "metadata requires proof");
  IR = program();
  replace(IR, "ret i64 %answer\n}", "ret i64 %answer\ndead:\nret i64 0\n}");
  refuses(IR, "unreachable blocks");
}

TEST(LLVMPrivateFrame, StorageBudgetIncludesEveryDataflowVector) {
  llvm::LLVMContext C;
  auto M = parse(C, program());
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  const auto Before = print(*M);
  LLVMPrivateFrameLimits Limits;
  // The 32 referenced bytes need one storage word in each vector.
  Limits.MaxDataflowBytes = 8 * (2 * F.size() + 1) - 1;
  auto R = projectLLVMPrivateFrame(F, contract(F), Limits);
  EXPECT_EQ(R.Status, Result::Unsupported);
  EXPECT_NE(R.Diagnostic.find("storage limit"), std::string::npos);
  EXPECT_EQ(print(*M), Before);
  ++Limits.MaxDataflowBytes;
  R = projectLLVMPrivateFrame(F, contract(F), Limits);
  EXPECT_EQ(R.Status, Result::Projected) << R.Diagnostic;
}

TEST(LLVMPrivateFrame, RetainsScalarPoisonObligationsAndNumericValues) {
  auto IR = program();
  replace(IR, "%plus = add i32 %x, 23", "%plus = add nuw i32 %x, 23");
  llvm::LLVMContext C;
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  llvm::SmallVector<llvm::Instruction *> Original;
  for (auto &I : llvm::instructions(F))
    Original.push_back(&I);
  auto R = projectLLVMPrivateFrame(F, contract(F));
  EXPECT_EQ(R.Status, Result::Projected) << R.Diagnostic;
  for (auto *I : Original)
    EXPECT_EQ(I->getFunction(), &F);
  EXPECT_NE(print(*M).find("add nuw i32 %x, 23"), std::string::npos);
  // Projection has not certified this overflow annotation as defined.
  EXPECT_NE(print(*M).find("xor i64 %both, %root"), std::string::npos);
}

TEST(LLVMPrivateFrame, ExactAndShortBudgetsAreTransactional) {
  uint64_t Required = 0;
  for (unsigned Pass = 0; Pass != 3; ++Pass) {
    llvm::LLVMContext C;
    auto M = parse(C, program());
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto Before = print(*M);
    LLVMPrivateFrameLimits Limits;
    if (Pass)
      Limits.MaxWork = Required - (Pass == 2);
    auto R = projectLLVMPrivateFrame(F, contract(F), Limits);
    if (!Pass)
      Required = R.Work;
    if (Pass != 2) {
      EXPECT_EQ(R.Status, Result::Projected) << R.Diagnostic;
      EXPECT_EQ(R.Work, Required);
    } else {
      EXPECT_EQ(R.Status, Result::WorkLimitExceeded) << R.Diagnostic;
      EXPECT_EQ(print(*M), Before);
    }
  }
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    llvm::LLVMContext C;
    auto M = parse(C, program());
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    LLVMPrivateFrameLimits Limits;
    switch (Variant) {
    case 0:
      Limits.MaxWork = 0;
      break;
    case 1:
      Limits.MaxBlocks = 0;
      break;
    case 2:
      Limits.MaxInstructions = 0;
      break;
    case 3:
      Limits.MaxFrameBytes = 63;
      break;
    case 4:
      Limits.MaxDataflowBytes = 0;
      break;
    }
    const auto Before = print(*M);
    auto R = projectLLVMPrivateFrame(F, contract(F), Limits);
    EXPECT_NE(R.Status, Result::Projected);
    EXPECT_EQ(print(*M), Before);
  }
}
} // namespace neverd::analysis::frame_test
