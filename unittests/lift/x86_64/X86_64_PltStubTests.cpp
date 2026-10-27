//===- X86_64_PltStubTests.cpp - Calls through x86-64 PLT entries ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A shared object calls its imports through their PLT entries, which the
// lift decompiles as functions forwarding to the import's slot.  The
// fixtures (test_plt_byte_write_x64.s, test_plt_pair_returns_x64.s) are
// linked by ld.lld.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringRef.h"
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

/// \p Img's HighC.
std::string decompile(const BinaryImage &Img, llvm::LLVMContext &Ctx) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success) {
    ADD_FAILURE() << Result.Error;
    return {};
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit(Result.HighFuncs, OS, Options))
    ADD_FAILURE() << "emission failed";
  return Source;
}

} // namespace

// plt_iszero keeps strcmp's zero test in DL.  `sete dl` reads RDX only to
// keep the bytes it does not write, so strcmp's PLT entry returns its one
// int, not a record of RAX and RDX it could never recover.
TEST(X86_64PltStub, AByteWriteAfterTheCallReadsNoSecondField) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_plt_byte_write_x64.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  const std::string Source = decompile(*Img, Ctx);
  ASSERT_FALSE(Source.empty());
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("struct "), std::string::npos) << Source;
}

// sum_pair reads both registers pair_of returns.  pair_of is preemptible, so
// the call goes through its PLT entry, whose slot the loader starts at this
// image's own pair_of: the entry forwards the pair, and pair_of returns it.
// pair_of7's entry also passes on the stack argument its caller stored.
TEST(X86_64PltStub, AnOwnRoutinesEntryForwardsItsRegisterPair) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_plt_pair_returns_x64.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  const std::string Source = decompile(*Img, Ctx);
  ASSERT_FALSE(Source.empty());
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  EXPECT_NE(Source.find("__int128 pair_of("), std::string::npos) << Source;
}

// high_quotient returns the high half of `n / d`, which __udivti3 returns in
// RDX: the call returns both registers though only RDX is read, its PLT
// entry forwards both, and C declares the routine with its double word.
TEST(X86_64PltStub, ADoubleWordRoutineReturnsBothRegisters) {
  const std::optional<BinaryImage> Img =
      loadFixture("test_plt_pair_returns_x64.so");
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  const std::string Source = decompile(*Img, Ctx);
  ASSERT_FALSE(Source.empty());
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern unsigned __int128 __udivti3("),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("void high_quotient("), std::string::npos) << Source;
  EXPECT_NE(Source.find(">> 64"), std::string::npos) << Source;
}
