//===- X86_32_PICLibraryTests.cpp - i386 position-independent libraries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An i386 shared library's PLT entry jumps through its GOT entry by the GOT
// address its caller holds in EBX (`jmp *disp(%ebx)`), not through an
// absolute slot.  The entry is the import whose slot that is.
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

TEST(X86_32PICLibrary, PLTEntryThroughTheGOTIsTheImport) {
  const std::filesystem::path Library =
      std::filesystem::path(TEST_OBJ_DIR) / "test_pic_library_i386.so";
  if (!std::filesystem::exists(Library))
    GTEST_SKIP() << "the fixture links with ld.lld";
  auto ImageOrErr = loadBinary(Library);
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const BinaryImage &Img = *ImageOrErr;
  const Section *Plt = Img.getPltSection();
  ASSERT_NE(Plt, nullptr);
  // The entry after PLT0.
  const Import *Imp = Img.findImportStubAt(Plt->VA + 16);
  ASSERT_NE(Imp, nullptr);
  EXPECT_EQ(Imp->Name, "fputs");

  const Symbol *Put = Img.findSymbol("put");
  ASSERT_NE(Put, nullptr);
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Put->Addr};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  EXPECT_NE(Source.find("fputs("), std::string::npos) << Source;
}

} // namespace
