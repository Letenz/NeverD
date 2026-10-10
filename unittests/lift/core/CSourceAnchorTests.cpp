#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

TEST(CSourceAnchors, HighStatementRequiresObservedAddressAndKind) {
  HighFunc Function;
  Function.Name = "mapped_return";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeInt(4);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.Addr = 0x1012;
  Return.RetVal = HighExpr::makeConst(7, 4);
  Function.Body = {Return};
  HighSourceMap Observations = {
      {0x1000, {0x1012, 3}, {}, HighSourceKind::Statement, StmtKind::Return}};
  CSourceMap Map;
  Map.HighSources = &Observations;
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Ordinary, Mapped;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OrdinaryOS, Options));
  Options.SourceMap = &Map;
  ASSERT_TRUE(HighCEmitter().emit({Function}, MappedOS, Options));
  EXPECT_EQ(Ordinary, Mapped);
  ASSERT_EQ(Map.Anchors.size(), 1u);
  EXPECT_EQ(Map.Anchors[0].Function, 0x1000u);
  EXPECT_EQ(Map.Anchors[0].Occurrences,
            (std::vector<sigs::LibraryOccurrence>{{0x1012, 3}}));
  EXPECT_NE(llvm::StringRef(Mapped)
                .slice(Map.Anchors[0].Span.Begin, Map.Anchors[0].Span.End)
                .find("return"),
            llvm::StringRef::npos);
  EXPECT_FALSE(Map.Anchors[0].Span.Begin == 0);

  auto emitAgain = [&] {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
    EXPECT_TRUE(Map.Anchors.empty());
    EXPECT_EQ(Text, Ordinary);
  };
  Observations[0].StatementKind = StmtKind::Assign;
  emitAgain();
  Observations[0].StatementKind = StmtKind::Return;
  Observations[0].Occurrence.Address = 0x1013;
  emitAgain();
  Observations[0].Occurrence.Address = 0x1012;
  Observations.push_back(
      {0x1000, {0x1012, 4}, {}, HighSourceKind::Statement, StmtKind::Return});
  emitAgain();
  Observations.pop_back();
  Function.Body[0].IsPhiCopy = true;
  emitAgain();
}

TEST(CSourceAnchors, LLVMObservationsSurviveOnlyWithTheirFunctionAndValue) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module =
      llvm::parseAssemblyString("define i32 @mapped_return(i32 %value) { %sum "
                                "= add i32 %value, 7 ret i32 %sum }",
                                Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  auto *Function = Module->getFunction("mapped_return");
  ASSERT_NE(Function, nullptr);
  auto *Return = Function->getEntryBlock().getTerminator();
  LLVMSourceMap Observations;
  Observations.Functions[0x2000] = Function;
  Observations.Observations.push_back({0x2000, {0x2010, 2}, Return});
  CSourceMap Map;
  Map.LLVMSources = &Observations;
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Ordinary, Mapped;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OrdinaryOS, Options));
  Options.SourceMap = &Map;
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, MappedOS, Options));
  EXPECT_EQ(Ordinary, Mapped);
  ASSERT_FALSE(Map.Anchors.empty());
  EXPECT_EQ(Map.Anchors[0].Occurrences[0].Address, 0x2010u);
  EXPECT_TRUE(llvm::StringRef(Mapped)
                  .slice(Map.Anchors[0].Span.Begin, Map.Anchors[0].Span.End)
                  .contains("return"));
  Observations.Observations[0].Function = 0x2001;
  std::string Refused;
  llvm::raw_string_ostream OS(Refused);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_EQ(Refused, Ordinary);
  EXPECT_TRUE(Map.Anchors.empty());
}
