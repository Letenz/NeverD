//===- CSourceDefinitionTests.cpp - Where emitted definitions begin -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {

HighFunc returnsConstant(const char *Name, va_t Entry, uint64_t Value) {
  HighFunc F;
  F.Name = Name;
  F.Entry = Entry;
  F.ReturnType = NdType::makeInt(8, false);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(Value, 8);
  F.Body = {Return};
  return F;
}

/// Every definition starts a line of \p Text, after a prelude.
void expectLineStarts(llvm::StringRef Text, const CSourceMap &Map) {
  for (const auto &Definition : Map.Definitions) {
    ASSERT_GT(Definition.Begin, 0u);
    ASSERT_LT(Definition.Begin, Text.size());
    EXPECT_EQ(Text[Definition.Begin - 1], '\n') << Text.str();
  }
}

TEST(CSourceDefinitions, HighCRecordsEachTopLevelDefinition) {
  const std::vector<HighFunc> Funcs = {returnsConstant("first", 0x1000, 1),
                                       returnsConstant("second", 0x2000, 2)};
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Ordinary, Mapped;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
  ASSERT_TRUE(HighCEmitter().emit(Funcs, OrdinaryOS, Options));
  CSourceMap Map;
  Options.SourceMap = &Map;
  ASSERT_TRUE(HighCEmitter().emit(Funcs, MappedOS, Options));
  // Recording never changes the text.
  EXPECT_EQ(Ordinary, Mapped);
  ASSERT_EQ(Map.Definitions.size(), 2u) << Mapped;
  EXPECT_EQ(Map.Definitions[0].Entry, std::optional<va_t>(0x1000));
  EXPECT_EQ(Map.Definitions[1].Entry, std::optional<va_t>(0x2000));
  expectLineStarts(Mapped, Map);
  const llvm::StringRef Text(Mapped);
  const auto First =
      Text.slice(Map.Definitions[0].Begin, Map.Definitions[1].Begin);
  EXPECT_TRUE(First.starts_with("/* neverd.entry: 0x1000 */\n")) << First.str();
  EXPECT_TRUE(First.contains("first(")) << First.str();
  EXPECT_FALSE(First.contains("second(")) << First.str();
  const auto Second = Text.drop_front(Map.Definitions[1].Begin);
  EXPECT_TRUE(Second.starts_with("/* neverd.entry: 0x2000 */\n"))
      << Second.str();
  EXPECT_TRUE(Second.contains("second(")) << Second.str();
  // The prelude holds the includes.
  EXPECT_TRUE(Text.take_front(Map.Definitions[0].Begin).contains("#include"))
      << Mapped;

  // A later emission replaces the definitions.
  std::string Single;
  llvm::raw_string_ostream SingleOS(Single);
  ASSERT_TRUE(HighCEmitter().emit({Funcs[1]}, SingleOS, Options));
  ASSERT_EQ(Map.Definitions.size(), 1u);
  EXPECT_EQ(Map.Definitions[0].Entry, std::optional<va_t>(0x2000));
}

TEST(CSourceDefinitions, LLVMCRecordsDefinitionsWithoutKnownEntries) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
define i64 @first(i64 %value) {
  %sum = add i64 %value, 1
  ret i64 %sum
}
define i64 @second(i64 %value) {
  %product = mul i64 %value, 3
  ret i64 %product
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Ordinary, Mapped;
  llvm::raw_string_ostream OrdinaryOS(Ordinary), MappedOS(Mapped);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OrdinaryOS, Options));
  CSourceMap Map;
  Options.SourceMap = &Map;
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, MappedOS, Options));
  EXPECT_EQ(Ordinary, Mapped);
  ASSERT_EQ(Map.Definitions.size(), 2u) << Mapped;
  // Without an LLVM source map nothing names the functions' entries.
  EXPECT_EQ(Map.Definitions[0].Entry, std::nullopt);
  EXPECT_EQ(Map.Definitions[1].Entry, std::nullopt);
  expectLineStarts(Mapped, Map);
  const llvm::StringRef Text(Mapped);
  EXPECT_TRUE(Text.slice(Map.Definitions[0].Begin, Map.Definitions[1].Begin)
                  .contains("first("))
      << Mapped;
  EXPECT_TRUE(Text.drop_front(Map.Definitions[1].Begin).contains("second("))
      << Mapped;
}

} // namespace
