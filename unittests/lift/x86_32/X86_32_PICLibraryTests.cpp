//===- X86_32_PICLibraryTests.cpp - i386 position-independent libraries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An i386 shared library's PLT entry jumps through its GOT entry by the GOT
// address its caller holds in EBX (`jmp *disp(%ebx)`), not through an
// absolute slot.  The entry is the import whose slot that is.  Its functions
// find that address by calling a get-PC thunk, which loads one register with
// the return address and leaves every other one as it was.
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
#include <optional>
#include <string>

using namespace neverd;

namespace {

std::optional<BinaryImage> loadLibrary() {
  const std::filesystem::path Library =
      std::filesystem::path(TEST_OBJ_DIR) / "test_pic_library_i386.so";
  if (!std::filesystem::exists(Library))
    return std::nullopt;
  auto ImageOrErr = loadBinary(Library);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

/// The HighC source of the library's function \p Name.
std::string decompile(const BinaryImage &Img, llvm::StringRef Name) {
  const Symbol *Function = Img.findSymbol(Name);
  if (!Function)
    return "no symbol " + Name.str();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Function->Addr};
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

TEST(X86_32PICLibrary, PLTEntryThroughTheGOTIsTheImport) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Section *Plt = Img->getPltSection();
  ASSERT_NE(Plt, nullptr);
  // The entry after PLT0.
  const Import *Imp = Img->findImportStubAt(Plt->VA + 16);
  ASSERT_NE(Imp, nullptr);
  EXPECT_EQ(Imp->Name, "fputs");
  const std::string Source = decompile(*Img, "put");
  EXPECT_NE(Source.find("fputs("), std::string::npos) << Source;
}

// Taken for an ordinary call, a get-PC thunk call kept the old value of the
// register it loads and lost %ecx, which `scale` and `twice` keep across it,
// so both the GOT address and the sum came out unknown.  `twice` calls a
// thunk padded with nop.
TEST(X86_32PICLibrary, GetPcThunkCallLoadsOnlyItsRegister) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  for (const char *Name : {"put", "scale", "twice"}) {
    SCOPED_TRACE(Name);
    const std::string Source = decompile(*Img, Name);
    EXPECT_EQ(Source.find("get_pc_thunk"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
    if (llvm::StringRef(Name) != "put")
      EXPECT_NE(Source.find("factor"), std::string::npos) << Source;
  }
}

} // namespace
