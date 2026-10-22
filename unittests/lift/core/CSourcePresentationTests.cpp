//===- CSourcePresentationTests.cpp - Source text and variable metadata ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source presentation retains storage widths and decodes image text.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

using namespace neverd;

namespace {

TEST(CSourcePresentation, AggregateStorageIsNotAnIntegerVariable) {
  for (llvm::StringRef Storage : {"[248 x i8]", "[16 x i8]", "{ i64, i64 }"}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    const std::string IR =
        "target datalayout = \"e-p:64:64-i64:64-n8:16:32:64-S128\"\n"
        "declare i64 @consume(ptr)\n"
        "define i64 @sample() {\n"
        "  %buffer = alloca " +
        Storage.str() +
        ", align 16\n"
        "  store i8 65, ptr %buffer\n"
        "  %value = call i64 @consume(ptr %buffer)\n"
        "  ret i64 %value\n}\n";
    auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    rewrite_source::setOriginalVA(*Module->getFunction("sample"), 0x1000);
    CSourceMap Map;
    CEmitterOptions Options;
    Options.SourceMap = &Map;
    Options.TheArch = Arch::X64;
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
    bool Found = false;
    for (const auto &[Key, Variable] : Map.Variables) {
      if (Variable.Bytes < 16)
        continue;
      Found = true;
      EXPECT_TRUE(Variable.Ambiguous);
      // Source-variable editing accepts scalar storage only. An allocation
      // keeps its byte extent without inventing an integer of that extent.
      EXPECT_FALSE(
          SourceVariableTypes::compatible(Variable.Type, Variable.Bytes));
      EXPECT_EQ(Variable.Bytes, Storage == "[248 x i8]" ? 248u : 16u);
    }
    EXPECT_TRUE(Found) << Text;
    for (const auto &[Key, Variable] : Map.Variables)
      if (SourceVariableTypes::compatible(Variable.Type, Variable.Bytes))
        EXPECT_NO_THROW((void)declarationToC(Variable.Type, ""));
  }
}

TEST(CSourcePresentation, WideImageReadsShowTextInBothCRoutes) {
  struct Case {
    std::vector<uint8_t> Bytes;
    const char *Text;
  };
  const std::string Long = "A copied string across vector loads";
  std::vector<Case> Cases;
  Cases.push_back({std::vector<uint8_t>(Long.begin(), Long.end()),
                   "\"A copied string across vector loads\""});
  for (unsigned Unit : {2u, 4u}) {
    for (bool Big : {false, true}) {
      std::vector<uint8_t> Bytes;
      for (unsigned char Ch : Long)
        for (unsigned I = 0; I < Unit; ++I)
          Bytes.push_back(I == (Big ? Unit - 1 : 0) ? Ch : 0);
      Cases.push_back(
          {std::move(Bytes), Unit == 2 ? (Big ? "UTF-16BE" : "UTF-16LE")
                                       : (Big ? "UTF-32BE" : "UTF-32LE")});
    }
  }
  const std::string Unicode = "中文字符 copied string";
  Cases.push_back({std::vector<uint8_t>(Unicode.begin(), Unicode.end()),
                   "中文字符 copied string"});
  Cases.push_back(
      {{0xd6, 0xd0, 0xce, 0xc4, 0xd7, 0xd6, 0xb7, 0xfb}, "GBK \"中文字符\""});
  for (const auto &Test : Cases) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Segment Data;
    Data.VA = 0x2000;
    Data.Name = ".rodata";
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Test.Bytes;
    Data.Data.resize(Data.Data.size() + 16, 0);
    Data.Size = Data.Data.size();
    Image.Segments.push_back(std::move(Data));
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "read_chunk";
    Function.ReturnType = NdType::makeInt(16, false);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal =
        HighExpr::makeLoad(HighExpr::makeConst(0x2000, 8), Function.ReturnType);
    Function.Body.push_back(std::move(Return));
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(R"(
      target datalayout = "e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"
      define i128 @read_chunk() {
        %value = load i128, ptr inttoptr (i64 8192 to ptr), align 16
        ret i128 %value
      }
    )",
                                            Diagnostic, Context);
    ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
    for (bool LLVM : {false, true}) {
      for (bool Comments : {true, false}) {
        CEmitterOptions Options;
        Options.TheArch = Image.Arch;
        Options.Format = Image.Format;
        Options.Image = &Image;
        Options.EmitComments = Comments;
        std::string Text;
        llvm::raw_string_ostream Stream(Text);
        ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Module, Stream, Options,
                                               nullptr, &Image)
                         : HighCEmitter().emit({Function}, Stream, Options));
        if (Comments)
          EXPECT_NE(Text.find(Test.Text), std::string::npos) << Text;
        else
          EXPECT_EQ(Text.find(Test.Text), std::string::npos) << Text;
      }
    }
  }
}

} // namespace
