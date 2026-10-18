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

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <optional>
#include <regex>
#include <string>

using namespace neverd;

namespace {

/// The fixture \p File, the assembled object or the library linked from it.
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

std::optional<BinaryImage> loadLibrary() {
  return loadFixture("test_pic_library_i386.so");
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

/// Expect neither of \p Img's call/pop functions to print the return address
/// its call pushed, and each to keep what it computes.
void expectNoGetPcPush(const BinaryImage &Img) {
  for (const char *Name : {"clang_scale", "clang_lower"}) {
    SCOPED_TRACE(Name);
    const Symbol *Pop = Img.findSymbol(std::string(Name) + "_pop");
    ASSERT_NE(Pop, nullptr);
    const std::string Source = decompile(Img, Name);
    // The writer spells the return address in uppercase hexadecimal.
    EXPECT_FALSE(std::regex_search(
        Source, std::regex("\\b0x" + llvm::utohexstr(Pop->Addr) + "\\b")))
        << Source;
    EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  }
  const std::string Scale = decompile(Img, "clang_scale");
  EXPECT_NE(Scale.find("factor"), std::string::npos) << Scale;
  // The byte the call passes keeps its zero extension.
  const std::string Lower = decompile(Img, "clang_lower");
  EXPECT_NE(Lower.find("(uint8_t)arg0"), std::string::npos) << Lower;
}

// Clang's call to the next instruction pushes the return address for the pop
// alone, which reads it as the PC.  A frame realigned to 16 bytes kept that
// push as a local of the C (`var_m24 = 0x146E;`), whether the pop's value
// was read, as `clang_scale` reads `factor` through it, or not: `clang_lower`
// keeps it in %ebx only for its PLT call.
TEST(X86_32PICLibrary, GetPcCallPushIsNoLocal) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  expectNoGetPcPush(*Img);
}

// The object those functions are linked from gives the pop's sum the GOT
// base, not the PC: `clang_scale`'s pop is read only by that sum, and its
// push stayed a local (`var_m24 = 0x7E;`).
TEST(X86_32PICLibrary, ObjectGetPcCallPushIsNoLocal) {
  const std::optional<BinaryImage> Img = loadFixture("test_pic_library_i386.o");
  if (!Img)
    GTEST_SKIP() << "the fixture assembles with clang";
  expectNoGetPcPush(*Img);
}

} // namespace
