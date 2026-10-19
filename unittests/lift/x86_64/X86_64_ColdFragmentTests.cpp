//===- X86_64_ColdFragmentTests.cpp - GCC .cold parts of a function -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// GCC's hot/cold partitioning moves a function's cleanup to `foo.cold`,
/// whose FDE begins inside the function's frame.  The fragment is the
/// function's own code, entered by a jump with its frame: a function of its
/// own reads the parent's registers as unknown values.
///
/// The fixture, test_gcc_cold_fragment_x64.s, is GCC 15.2's output for a
/// cleanup that destroys a local before _Unwind_Resume, linked with its
/// symbols and stripped.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <optional>
#include <string>

using namespace neverd;

namespace {

/// The fixture \p File, linked by ld.lld.
std::optional<BinaryImage> loadFixture(llvm::StringRef File) {
  const std::filesystem::path Path =
      std::filesystem::path(TEST_OBJ_DIR) / File.str();
  if (!std::filesystem::exists(Path))
    return std::nullopt;
  auto ImageOrErr = loadBinary(Path);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

/// The HighC source of every function of \p Img.
std::string decompileAll(const BinaryImage &Img) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success)
    return "pipeline failed: " + Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit(Result.HighFuncs, OS, Options))
    return "emission failed";
  return Source;
}

/// The definition of \p Name in \p Source, or empty.
std::string definitionOf(const std::string &Source, llvm::StringRef Name) {
  const std::string Head = " " + Name.str() + "(";
  size_t At = 0;
  while ((At = Source.find(Head, At)) != std::string::npos) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd != std::string::npos && LineEnd > 0 &&
        Source[LineEnd - 1] == '{') {
      const size_t Begin = Source.rfind('\n', At) + 1;
      return Source.substr(Begin, Source.find("\n}\n", At) - Begin);
    }
    At += Head.size();
  }
  return {};
}

/// The start of `gcc_cold_guard.cold`, which the linked copy names.
std::optional<va_t> coldStart() {
  const std::optional<BinaryImage> Named =
      loadFixture("test_gcc_cold_fragment_x64.so");
  if (!Named)
    return std::nullopt;
  const Symbol *Cold = Named->findSymbol("gcc_cold_guard.cold");
  if (!Cold)
    return std::nullopt;
  return Cold->Addr;
}

// The loader binds the fragment to the function its landing pad jumps from,
// as an x64 chained unwind record is bound to its primary.
TEST(X86_64ColdFragment, TheLoaderBindsTheColdPartToItsFunction) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_gcc_cold_fragment_x64_stripped.so");
  const std::optional<va_t> Cold = coldStart();
  if (!Img || !Cold)
    GTEST_SKIP() << "the fixture links with ld.lld";
  EXPECT_TRUE(Img->ContinuationCodeStarts.count(*Cold));
  const ExceptionFunction *Fragment =
      Img->ExceptionMetadata.findFunctionEntry(*Cold);
  ASSERT_NE(Fragment, nullptr);
  EXPECT_EQ(Fragment->Kind, RuntimeFunctionKind::Fragment);
  const Symbol *Parent = Img->findSymbol("gcc_cold_guard");
  ASSERT_NE(Parent, nullptr);
  const ExceptionFunction *Primary =
      Img->ExceptionMetadata.findFunctionEntry(Parent->Addr);
  ASSERT_NE(Primary, nullptr);
  ASSERT_EQ(Primary->FragmentRanges.size(), 1u);
  EXPECT_EQ(Primary->FragmentRanges[0].Begin, *Cold);
}

// Stripped, the cold part is no function: gcc_cold_guard's landing pad runs
// its cleanup, which destroys `g` and resumes unwinding with the exception
// object.  As a function of its own, the part read RBX and RBP unknown, and
// gcc_cold_guard, whose cleanup path ended in a call of it, returned no value.
TEST(X86_64ColdFragment, AStrippedColdPartIsItsFunctionsCode) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_gcc_cold_fragment_x64_stripped.so");
  const std::optional<va_t> Cold = coldStart();
  if (!Img || !Cold)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source = decompileAll(*Img);
  const std::string Parent = definitionOf(Source, "gcc_cold_guard");
  ASSERT_FALSE(Parent.empty()) << Source;
  EXPECT_EQ(Parent.rfind("void ", 0), std::string::npos) << Source;
  EXPECT_NE(Parent.find("_Unwind_Resume("), std::string::npos) << Source;
  EXPECT_EQ(Parent.find("unknown"), std::string::npos) << Source;
  EXPECT_TRUE(definitionOf(Source, "sub_" + llvm::utohexstr(*Cold)).empty())
      << Source;
}

// With its symbols, the part `gcc_cold_guard.cold` names is the function's
// code too, and no definition of its own.
TEST(X86_64ColdFragment, ANamedColdPartIsItsFunctionsCode) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_gcc_cold_fragment_x64.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source = decompileAll(*Img);
  const std::string Parent = definitionOf(Source, "gcc_cold_guard");
  ASSERT_FALSE(Parent.empty()) << Source;
  EXPECT_NE(Parent.find("_Unwind_Resume("), std::string::npos) << Source;
  EXPECT_EQ(Parent.find("unknown"), std::string::npos) << Source;
  EXPECT_TRUE(definitionOf(Source, "gcc_cold_guard_cold").empty()) << Source;
  // The function list holds no function the name begins.
  const Symbol *Cold = Img->findSymbol("gcc_cold_guard.cold");
  ASSERT_NE(Cold, nullptr);
  EXPECT_TRUE(llvm::none_of(Img->getFunctionSymbols(), [&](const Symbol *Sym) {
    return Sym->Addr == Cold->Addr;
  }));
}

} // namespace
