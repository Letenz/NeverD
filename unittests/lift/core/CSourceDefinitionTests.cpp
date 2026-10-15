//===- CSourceDefinitionTests.cpp - Where emitted definitions begin -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/CSourceRecorder.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>
#include <stdexcept>

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

class RecordingDebugContext : public NullDebugContext {
public:
  mutable size_t Queries = 0;
  size_t FailAt = 0;
  bool Throw = false;
  mutable bool Failed = false;

  std::optional<FunctionSym> resolveFunction(va_t) const override {
    if (++Queries != FailAt)
      return std::nullopt;
    Failed = true;
    if (Throw)
      throw std::runtime_error("private rendering failed");
    // Change only this render's declared return width. The ordinary render
    // and subsequent independent renders still see the original declaration.
    FunctionSym Function;
    Function.ReturnType = NdType::makeInt(4, false);
    return Function;
  }
};

TEST(CSourceDefinitions, CombinedRecordingPublishesOnlyByteExactLocations) {
  HighFunc Function = returnsConstant("combined", 0x1000, 7);
  Function.Body[0].Addr = 0x1010;
  HighSourceMap Sources = {
      {0x1000, {0x1010, 2}, {}, HighSourceKind::Statement, StmtKind::Return}};
  CSourceMap Map;
  Map.HighSources = &Sources;
  // The marker-looking user text must remain ordinary text, including UTF-8.
  const std::string Ordinary = "/* \x1eND:0: user 文本 */\nreturn 7;\n";
  CSourceRecorder Recorder(
      Map, Ordinary, CSourceRecorder::Recording::DefinitionsAndInstructions);
  Recorder.prepareHighSources();
  auto Event = Recorder.statement(Function.Entry, Function.Body[0]);
  ASSERT_TRUE(Event);
  const std::string Annotated = Recorder.definition(Function.Entry) +
                                Recorder.begin(*Event) + Ordinary +
                                Recorder.end(*Event);
  ASSERT_TRUE(Recorder.finish(Annotated, Ordinary));
  EXPECT_EQ(Map.Definitions, (std::vector<CSourceDefinition>{{0x1000, 0}}));
  ASSERT_EQ(Map.Anchors.size(), 1u);
  EXPECT_EQ(Map.Anchors[0].Span, (CSourceSpan{0, Ordinary.size()}));
  EXPECT_EQ(Map.Anchors[0].Occurrences,
            (std::vector<sigs::LibraryOccurrence>{{0x1010, 2}}));
  EXPECT_TRUE(Map.Regions.empty());

  CSourceRecorder Changed(
      Map, Ordinary, CSourceRecorder::Recording::DefinitionsAndInstructions);
  Changed.prepareHighSources();
  Event = Changed.statement(Function.Entry, Function.Body[0]);
  ASSERT_TRUE(Event);
  EXPECT_FALSE(Changed.finish(Changed.definition(Function.Entry) +
                                  Changed.begin(*Event) + "changed\n" +
                                  Changed.end(*Event),
                              Ordinary));
  EXPECT_TRUE(Map.Definitions.empty());
  EXPECT_TRUE(Map.Anchors.empty());
}

TEST(CSourceDefinitions, CombinedRecordingRefusesLibraryEvidence) {
  std::vector<sigs::LibraryRecognition> Recognitions(1);
  CSourceMap Map;
  Map.Recognitions = &Recognitions;
  Map.Definitions.push_back({0x1000, 17});
  EXPECT_THROW(
      CSourceRecorder(Map, "source",
                      CSourceRecorder::Recording::DefinitionsAndInstructions),
      std::invalid_argument);
  EXPECT_EQ(Map.Definitions, (std::vector<CSourceDefinition>{{0x1000, 17}}));
  EXPECT_THROW(
      CSourceRecorder(Map, "source", CSourceRecorder::Recording::Positions),
      std::invalid_argument);
  EXPECT_EQ(Map.Definitions, (std::vector<CSourceDefinition>{{0x1000, 17}}));
}

TEST(CSourceDefinitions, PositionRecordingRefusesCallExpressionCoverage) {
  auto Call = HighExpr::makeCall("ordinary_service", 0x2000, {});
  HighSourceMap Sources = {
      {0x1000, {0x1010, 2}, Call, HighSourceKind::CallExpression, {}}};
  CSourceMap Map;
  Map.HighSources = &Sources;
  Map.Definitions.push_back({0x1000, 17});
  EXPECT_THROW(
      CSourceRecorder(Map, "source", CSourceRecorder::Recording::Positions),
      std::invalid_argument);
  EXPECT_EQ(Map.Definitions, (std::vector<CSourceDefinition>{{0x1000, 17}}));
  EXPECT_NO_THROW(CSourceRecorder(
      Map, "source", CSourceRecorder::Recording::DefinitionsAndInstructions));
}

TEST(CSourceDefinitions, HighCEmptyLibraryEvidenceAvoidsAnExtraRender) {
  HighFunc Function = returnsConstant("one_render", 0x1000, 7);
  Function.Body[0].Addr = 0x1010;
  HighSourceMap Sources = {
      {0x1000, {0x1010, 2}, {}, HighSourceKind::Statement, StmtKind::Return}};
  std::vector<CSourceName> Names;
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.SourceNames = &Names;
  RecordingDebugContext Debug;
  std::string Ordinary;
  llvm::raw_string_ostream OS(Ordinary);
  ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options, &Debug));
  const size_t QueriesPerRender = Debug.Queries;
  ASSERT_GT(QueriesPerRender, 0u);
  const auto OrdinaryNames = Names;

  CSourceMap Map;
  Map.HighSources = &Sources;
  Options.SourceMap = &Map;
  std::vector<sigs::LibraryRecognition> Empty;
  std::vector<CSourceDefinition> Definitions;
  std::vector<CSourceAnchor> Anchors;
  for (bool HasCatalog : {false, true}) {
    SCOPED_TRACE(HasCatalog);
    Map.Recognitions = HasCatalog ? &Empty : nullptr;
    Debug.Queries = 0;
    std::string Mapped;
    llvm::raw_string_ostream MappedOS(Mapped);
    ASSERT_TRUE(HighCEmitter().emit({Function}, MappedOS, Options, &Debug));
    EXPECT_EQ(Mapped, Ordinary);
    EXPECT_EQ(Debug.Queries, QueriesPerRender);
    EXPECT_EQ(Names, OrdinaryNames);
    EXPECT_EQ(Map.Names, OrdinaryNames);
    ASSERT_EQ(Map.Definitions.size(), 1u);
    ASSERT_EQ(Map.Anchors.size(), 1u);
    Definitions = Map.Definitions;
    Anchors = Map.Anchors;
  }

  // A real whole-function library mapping keeps its independent render and
  // exactly the same definition/instruction locations.
  sigs::LibraryRecognition Match;
  Match.Function = Function.Entry;
  Match.Isolated = true;
  Match.Scope = sigs::LibraryFeatureScope::WholeFunction;
  Match.Occurrences = {{0x1010, 2}};
  const std::vector<sigs::LibraryRecognition> Recognitions = {Match};
  Map.Recognitions = &Recognitions;
  Debug.Queries = 0;
  std::string Mapped;
  llvm::raw_string_ostream MappedOS(Mapped);
  ASSERT_TRUE(HighCEmitter().emit({Function}, MappedOS, Options, &Debug));
  EXPECT_EQ(Mapped, Ordinary);
  EXPECT_EQ(Debug.Queries, 3 * QueriesPerRender);
  EXPECT_EQ(Map.Definitions, Definitions);
  ASSERT_EQ(Map.Anchors.size(), Anchors.size());
  EXPECT_EQ(Map.Anchors[0].Function, Anchors[0].Function);
  EXPECT_EQ(Map.Anchors[0].Span, Anchors[0].Span);
  EXPECT_EQ(Map.Anchors[0].Occurrences, Anchors[0].Occurrences);
  ASSERT_EQ(Map.Regions.size(), 1u);
  EXPECT_TRUE(Map.Regions[0].Mapped);
  EXPECT_FALSE(Map.Regions[0].Spans.empty());
}

TEST(CSourceDefinitions, HighCNavigationDoesNotRepeatTheOrdinaryRender) {
  for (bool Throw : {false, true}) {
    SCOPED_TRACE(Throw);
    HighFunc Function = returnsConstant("fallback", 0x1000, 7);
    Function.Body[0].Addr = 0x1010;
    HighSourceMap Sources = {
        {0x1000, {0x1010, 2}, {}, HighSourceKind::Statement, StmtKind::Return}};
    std::vector<CSourceName> Names;
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.SourceNames = &Names;
    RecordingDebugContext Debug;
    std::string Ordinary;
    llvm::raw_string_ostream OS(Ordinary);
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options, &Debug));
    Debug.FailAt = Debug.Queries + 1;
    Debug.Queries = 0;
    Debug.Throw = Throw;
    CSourceMap Map;
    Map.HighSources = &Sources;
    Options.SourceMap = &Map;
    std::string Mapped;
    llvm::raw_string_ostream MappedOS(Mapped);
    ASSERT_TRUE(HighCEmitter().emit({Function}, MappedOS, Options, &Debug));
    ASSERT_FALSE(Debug.Failed);
    EXPECT_EQ(Mapped, Ordinary);
    ASSERT_EQ(Map.Definitions.size(), 1u);
    EXPECT_EQ(Map.Definitions[0].Entry, std::optional<va_t>(0x1000));
    expectLineStarts(Mapped, Map);
    ASSERT_EQ(Map.Anchors.size(), 1u);
    EXPECT_EQ(Map.Anchors[0].Occurrences,
              (std::vector<sigs::LibraryOccurrence>{{0x1010, 2}}));
    EXPECT_TRUE(llvm::StringRef(Mapped)
                    .slice(Map.Anchors[0].Span.Begin, Map.Anchors[0].Span.End)
                    .contains("return"));
  }
}

TEST(CSourceDefinitions, OrdinaryRenderFailureIsNotRetriedAsMappingFailure) {
  RecordingDebugContext Debug;
  Debug.FailAt = 1;
  Debug.Throw = true;
  CSourceMap Map;
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.SourceMap = &Map;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_THROW(HighCEmitter().emit({returnsConstant("failure", 0x1000, 7)}, OS,
                                   Options, &Debug),
               std::runtime_error);
  EXPECT_EQ(Debug.Queries, 1u);
  EXPECT_TRUE(Text.empty());
  EXPECT_TRUE(Map.Definitions.empty());
  EXPECT_TRUE(Map.Anchors.empty());
}

TEST(CSourceDefinitions, PositionsFollowNestedBuffersAndKeepUserTextPlain) {
  auto Function = returnsConstant("buffered", 0x1000, 7);
  Function.Body[0].Addr = 0x1010;
  HighSourceMap Sources = {
      {0x1000, {0x1010, 2}, {}, HighSourceKind::Statement, StmtKind::Return}};
  CSourceMap Map;
  Map.HighSources = &Sources;
  CSourceRecorder Recorder(Map, {}, CSourceRecorder::Recording::Positions);
  Recorder.prepareHighSources();
  const auto Event = Recorder.statement(Function.Entry, Function.Body[0]);
  ASSERT_TRUE(Event);
  const std::string Prelude = "/* \x1eND:0:0B\x1f 文本 */\n";
  const std::string BodyPrefix = "{\n  uint64_t local;\n";
  const std::string Inner = "  return 7;\n";
  const std::string Ordinary = Prelude + BodyPrefix + Inner + "}\n";
  std::string Scratch;
  EXPECT_EQ(Recorder.plain(Prelude, Scratch), Prelude);
  EXPECT_EQ(
      Recorder.expression(Function.Entry, *Function.Body[0].RetVal, Prelude),
      Prelude);
  Recorder.positionDefinition(Function.Entry, Prelude.size());
  const auto Outer = Recorder.positionBuffer();
  const auto Nested = Recorder.positionBuffer();
  Recorder.positionStatement(*Event, 0, Inner.size());
  // A suppressed statement must not map the adjacent printed statement.
  Recorder.positionStatement(*Event, Inner.size(), Inner.size());
  Recorder.appendPositionBuffer(Nested, Inner.size(), BodyPrefix.size());
  Recorder.appendPositionBuffer(Outer, BodyPrefix.size() + Inner.size() + 2,
                                Prelude.size());
  EXPECT_TRUE(Map.Definitions.empty());
  EXPECT_TRUE(Map.Anchors.empty());
  ASSERT_TRUE(Recorder.finishPositions(Ordinary));
  EXPECT_EQ(Map.Definitions,
            (std::vector<CSourceDefinition>{{0x1000, Prelude.size()}}));
  ASSERT_EQ(Map.Anchors.size(), 1u);
  const auto &Anchor = Map.Anchors[0];
  EXPECT_EQ(Anchor.Function, 0x1000u);
  EXPECT_EQ(Anchor.Occurrences,
            (std::vector<sigs::LibraryOccurrence>{{0x1010, 2}}));
  EXPECT_EQ(llvm::StringRef(Ordinary).slice(Anchor.Span.Begin, Anchor.Span.End),
            Inner);
}

TEST(CSourceDefinitions, InvalidPositionsCannotPublishPartialNavigation) {
  for (unsigned Invalid = 0; Invalid < 6; ++Invalid) {
    SCOPED_TRACE(Invalid);
    auto Function = returnsConstant("invalid", 0x1000, 7);
    Function.Body[0].Addr = 0x1010;
    HighSourceMap Sources = {
        {0x1000, {0x1010, 2}, {}, HighSourceKind::Statement, StmtKind::Return}};
    CSourceMap Map;
    Map.HighSources = &Sources;
    CSourceRecorder Recorder(Map, {}, CSourceRecorder::Recording::Positions);
    Recorder.prepareHighSources();
    auto Event = Recorder.statement(Function.Entry, Function.Body[0]);
    ASSERT_TRUE(Event);
    Recorder.positionDefinition(Function.Entry, 0);
    Recorder.positionStatement(*Event, 0, 4);
    switch (Invalid) {
    case 0:
      Recorder.positionStatement(*Event, 4, 3);
      break;
    case 1:
      Recorder.positionStatement(*Event + 1, 0, 4);
      break;
    case 2:
      Recorder.appendPositionBuffer(2, 4, 0);
      break;
    case 3:
      Recorder.appendPositionBuffer(0, 3, 0);
      break;
    case 4:
      Recorder.appendPositionBuffer(0, 4, std::numeric_limits<uint64_t>::max());
      break;
    case 5:
      Recorder.positionDefinition(Function.Entry, 5);
      break;
    }
    EXPECT_FALSE(Recorder.finishPositions("text"));
    EXPECT_TRUE(Map.Anchors.empty());
    EXPECT_TRUE(Map.Definitions.empty());
  }
}

TEST(CSourceDefinitions, BufferedNestedStatementsMatchIndependentMarkerProof) {
  auto Function = returnsConstant("nested", 0x1000, 7);
  Function.Body[0].Addr = 0x1010;
  HighStmt Conditional;
  Conditional.Kind = StmtKind::IfElse;
  Conditional.Addr = 0x1005;
  Conditional.Cond = HighExpr::makeConst(1, 1);
  Conditional.Body = Function.Body;
  Conditional.ElseBody = Function.Body;
  Conditional.ElseBody[0].Addr = 0x1020;
  Conditional.ElseBody[0].RetVal = HighExpr::makeConst(11, 8);
  Function.Body = {Conditional};
  auto Second = returnsConstant("second", 0x2000, 9);
  Second.Body[0].Addr = 0x2010;
  HighSourceMap Sources;
  for (auto [Entry, Address, Kind] :
       {std::tuple{0x1000, 0x1005, StmtKind::IfElse},
        std::tuple{0x1000, 0x1010, StmtKind::Return},
        std::tuple{0x1000, 0x1020, StmtKind::Return},
        std::tuple{0x2000, 0x2010, StmtKind::Return}})
    Sources.push_back(
        {va_t(Entry), {va_t(Address), 0}, {}, HighSourceKind::Statement, Kind});
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  CSourceMap Direct, Marked;
  Direct.HighSources = Marked.HighSources = &Sources;
  sigs::LibraryRecognition Unrelated;
  Unrelated.Function = 0x9000;
  const std::vector<sigs::LibraryRecognition> Library = {Unrelated};
  Marked.Recognitions = &Library;
  std::string DirectText, MarkedText;
  llvm::raw_string_ostream DirectOS(DirectText), MarkedOS(MarkedText);
  Options.SourceMap = &Direct;
  ASSERT_TRUE(HighCEmitter().emit({Function, Second}, DirectOS, Options));
  Options.SourceMap = &Marked;
  ASSERT_TRUE(HighCEmitter().emit({Function, Second}, MarkedOS, Options));
  EXPECT_EQ(DirectText, MarkedText);
  EXPECT_EQ(Direct.Definitions, Marked.Definitions);
  ASSERT_EQ(Direct.Anchors.size(), 4u);
  ASSERT_EQ(Direct.Anchors.size(), Marked.Anchors.size());
  for (size_t I = 0; I < Direct.Anchors.size(); ++I) {
    EXPECT_EQ(Direct.Anchors[I].Span, Marked.Anchors[I].Span);
    EXPECT_EQ(Direct.Anchors[I].Function, Marked.Anchors[I].Function);
    EXPECT_EQ(Direct.Anchors[I].Occurrences, Marked.Anchors[I].Occurrences);
  }
}

TEST(CSourceDefinitions, SourceOutputFailureIsNotRetriedAsMappingFailure) {
  class FailingSourceStream : public llvm::raw_ostream {
  public:
    FailingSourceStream() { SetUnbuffered(); }
    unsigned Writes = 0;

  private:
    void write_impl(const char *, size_t) override {
      ++Writes;
      throw std::runtime_error("source output failed");
    }
    uint64_t current_pos() const override { return 0; }
  };
  CSourceMap Map;
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.SourceMap = &Map;
  FailingSourceStream Out;
  EXPECT_THROW(
      HighCEmitter().emit({returnsConstant("stream_failure", 0x1000, 7)}, Out,
                          Options),
      std::runtime_error);
  EXPECT_EQ(Out.Writes, 1u);
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
