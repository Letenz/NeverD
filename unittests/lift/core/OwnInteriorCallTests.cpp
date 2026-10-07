//===- OwnInteriorCallTests.cpp - Calls into a function's own body ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Hand-written x64 code calls labels inside its own unwind range, often only
// for the return address the call pushes.  Such a call is lifted as a push and
// a jump unless the target is proven to be a subroutine that returns to it,
// and a return that may pop a pushed address refuses the function.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Entry = 0x140001000;

BinaryImage makeImage(std::vector<uint8_t> Code) {
  const va_t End = Entry + Code.size();
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  // The `.pdata` record the COFF loader produces for the function.
  ExceptionFunction Own;
  Own.CodeRange = {Entry, End};
  Own.FunctionEntry = Entry;
  Own.Kind = RuntimeFunctionKind::Primary;
  Own.Encoding = ExceptionEncoding::X64UnwindV1;
  Img.ExceptionMetadata.Functions.push_back(Own);
  Img.ExceptionMetadata.rebuildIndex();
  Img.KnownCodeRanges.push_back({Entry, End});
  return Img;
}

struct Lifted {
  std::string Source;
  std::vector<va_t> Unsupported;
  std::vector<va_t> UnprovenReturns;
};

Lifted lift(std::vector<uint8_t> Code) {
  BinaryImage Img = makeImage(std::move(Code));
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  Lifted Out;
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
    if (Audit.Entry == Entry) {
      Out.Unsupported = Audit.UnsupportedInstructions;
      Out.UnprovenReturns = Audit.UnprovenReturns;
    }
  for (const HighFunc &Func : Result.HighFuncs) {
    if (Func.Entry != Entry)
      continue;
    llvm::raw_string_ostream OS(Out.Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  }
  return Out;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-own-call", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-own-call", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-own-call", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined,address",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

constexpr const char *ReturnsSeven = R"(
int main(void) { return sub_140001000() == 7 ? 0 : 1; }
)";

TEST(OwnInteriorCall, CallsForTheirReturnAddressPushAndJump) {
  // Each call skips an int3 to the next instruction; the stack pointer is
  // then restored past both return addresses.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100A
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001010
                         0xcc,                         // int3
                         0x48, 0x83, 0xc4, 0x10,       // add  rsp, 10h
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3});                       // ret
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_EQ(L.Source.find("sub_14000100A"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("sub_140001010"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("unknown value"), std::string::npos) << L.Source;
  compileAndRun(L.Source + ReturnsSeven);
}

TEST(OwnInteriorCall, LoopedPushesRestoredThroughTheFramePointer) {
  // The pushes go deeper on every iteration; the frame pointer restores the
  // stack pointer above all of them.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xb9, 0x02, 0x00, 0x00, 0x00, // mov  ecx, 2
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100F
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001015
                         0xcc,                         // int3
                         0xff, 0xc9,                   // dec  ecx
                         0x75, 0xf0,                   // jnz  0x140001009
                         0x48, 0x89, 0xec,             // mov  rsp, rbp
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3});                       // ret
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_EQ(L.Source.find("sub_14000100F"), std::string::npos) << L.Source;
  compileAndRun(L.Source + ReturnsSeven);
}

TEST(OwnInteriorCall, SubroutineThatReturnsStaysACall) {
  std::vector<uint8_t> Code = {0xe8, 0x0b, 0x00, 0x00, 0x00, // call 0x140001010
                               0x83, 0xc0, 0x01,             // add  eax, 1
                               0xc3};                        // ret
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0xb8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
                           0xc3});                       // ret
  const Lifted L = lift(std::move(Code));
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_NE(L.Source.find("sub_140001010()"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("return 5"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, ReturnBelowTheEntryStackPointerIsRefused) {
  // Only one of the two return addresses is discarded, so `pop rbp` takes
  // the other and `ret` pops the saved frame pointer.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100A
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001010
                         0xcc,                         // int3
                         0x48, 0x83, 0xc4, 0x08,       // add  rsp, 8
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3});                       // ret
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x14000101A});
  EXPECT_NE(
      std::find(L.Unsupported.begin(), L.Unsupported.end(), va_t{0x14000101A}),
      L.Unsupported.end());
}

TEST(OwnInteriorCall, ReturnAfterAStackSwitchIsRefused) {
  // The stack pointer comes from memory before the return, so nothing shows
  // the pushed return address is not what it pops.
  const Lifted L = lift({0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001006
                         0xcc,                         // int3
                         0x48, 0x8b, 0x21,             // mov  rsp, [rcx]
                         0xc3});                       // ret
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x140001009});
}

} // namespace
