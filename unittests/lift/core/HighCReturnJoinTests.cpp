//===- HighCReturnJoinTests.cpp - Results that reach a joined return ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A RETURN whose block joins paths that all leave the same version of the
// return register returns that value: it was established before the paths
// parted, here by a call whose result outlives a branch around a store.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <string>

using namespace neverd;

namespace {

/// The body of \p Name's definition in \p Source, or empty.
std::string definitionBody(const std::string &Source, const std::string &Name) {
  for (size_t At = Source.find(" " + Name + "("); At != std::string::npos;
       At = Source.find(" " + Name + "(", At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || LineEnd == 0 ||
        Source[LineEnd - 1] != '{')
      continue;
    const size_t End = Source.find("\n}\n", LineEnd);
    return Source.substr(LineEnd,
                         End == std::string::npos ? End : End - LineEnd);
  }
  return {};
}

TEST(HighCReturnJoin, ResultEstablishedBeforeABranchIsReturned) {
  for (const char *Object :
       {"test_return_join.o", "test_return_join_i386.o",
        "test_return_join_arm.o", "test_return_join_a64.o"}) {
    SCOPED_TRACE(Object);
    auto ImageOrErr = loadBinary(std::filesystem::path(TEST_OBJ_DIR) / Object);
    ASSERT_TRUE(static_cast<bool>(ImageOrErr))
        << llvm::toString(ImageOrErr.takeError());
    const BinaryImage &Img = *ImageOrErr;
    const Symbol *Entry = Img.findSymbol("call_then_store");
    ASSERT_NE(Entry, nullptr);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries = {Entry->Addr};
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
    EXPECT_EQ(Source.find("void call_then_store("), std::string::npos)
        << Source;
    const std::string Body = definitionBody(Source, "call_then_store");
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_NE(Body.find("triple("), std::string::npos) << Body;
    EXPECT_EQ(Body.find("return;"), std::string::npos) << Body;
  }
}

} // namespace
