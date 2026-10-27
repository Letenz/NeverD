//===- OwnInteriorCallTests.cpp - Calls into a function's own body ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Calls to labels inside a function's own unwind range can return normally
// or abandon their invocation. Each admitted return retains its physical PC
// ownership; an unproved internal or parent return refuses the function.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
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

/// The image a function's own unwind record comes from: its architecture and
/// format, the record's encoding as that format's loader produces it, and
/// where the function starts.
struct Platform {
  Arch TheArch;
  BinaryFormat Format;
  ExceptionEncoding Encoding;
  va_t Entry;
};

/// Microsoft x64, with the function's `.pdata` entry.
constexpr Platform Win64 = {Arch::X64, BinaryFormat::COFF,
                            ExceptionEncoding::X64UnwindV1, Entry};
/// System V x86-64, with the function's DWARF FDE.
constexpr Platform SysV64 = {Arch::X64, BinaryFormat::ELF,
                             ExceptionEncoding::DwarfFDE, Entry};
/// i386 ELF, with the function's DWARF FDE.
constexpr Platform I386 = {Arch::X86, BinaryFormat::ELF,
                           ExceptionEncoding::DwarfFDE, 0x8049000};
/// i386 PE, whose range is recovered from the `_except_handler3`
/// registration the prologue installs.
constexpr Platform I386Registration = {Arch::X86, BinaryFormat::COFF,
                                       ExceptionEncoding::X86ScopeTableEH3,
                                       0x401000};

BinaryImage makeImage(std::vector<uint8_t> Code, const Platform &P) {
  const va_t End = P.Entry + Code.size();
  BinaryImage Img;
  Img.Arch = P.TheArch;
  Img.Bits = P.TheArch == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Img.Format = P.Format;
  Img.Base = P.Entry - 0x1000;
  Img.Entry = P.Entry;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = P.Entry;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  ExceptionFunction Own;
  Own.CodeRange = {P.Entry, End};
  Own.FunctionEntry = P.Entry;
  Own.Kind = RuntimeFunctionKind::Primary;
  Own.Encoding = P.Encoding;
  Img.ExceptionMetadata.Functions.push_back(Own);
  Img.ExceptionMetadata.rebuildIndex();
  Img.KnownCodeRanges.push_back({P.Entry, End});
  return Img;
}

struct Lifted {
  std::string Source;
  std::vector<va_t> Unsupported;
  std::vector<va_t> UnprovenReturns;
};

Lifted lift(std::vector<uint8_t> Code, const Platform &P = Win64) {
  BinaryImage Img = makeImage(std::move(Code), P);
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(P.Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  Lifted Out;
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
    if (Audit.Entry == P.Entry) {
      Out.Unsupported = Audit.UnsupportedInstructions;
      Out.UnprovenReturns = Audit.UnprovenReturns;
    }
  for (const HighFunc &Func : Result.HighFuncs) {
    if (Func.Entry != P.Entry)
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

TEST(OwnInteriorCall, ClosedRecursiveNearCallsPreserveEachInvocation) {
  for (const Platform &P : {Win64, SysV64}) {
    // Two mutually recursive internal routines. The second decrements the
    // argument and tail-enters the first until zero; each ordinary RET then
    // consumes precisely its own CALL's PC.
    std::vector<uint8_t> Code = {0xB9, 5,    0, 0,   0, // mov ecx,5
                                 0xE8, 6,    0, 0,   0, // call +0x10
                                 0x83, 0xC0, 7, 0xC3};  // add eax,7; ret
    Code.resize(0x10, 0xCC);
    Code.insert(Code.end(), {0xE8, 0x0B, 0, 0, 0, 0xC3}); // call +0x20; ret
    Code.resize(0x20, 0xCC);
    Code.insert(Code.end(), {0x31, 0xC0,    // xor eax,eax
                             0x83, 0xE9, 1, // sub ecx,1
                             0x75, 0xE9,    // jne +0x10
                             0xC3});        // ret
    BinaryImage Image = makeImage(Code, P);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Low = CFGBuilder().build(Image, Dec, P.Entry);
    EXPECT_TRUE(Low.hasCompleteLiftCoverage());
    EXPECT_TRUE(Low.UnprovenReturnAddresses.empty());
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<int>(P.Format));
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        Options.OnlyFunctionEntries = {P.Entry, P.Entry + 0x10, P.Entry + 0x20};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = P.TheArch;
        COptions.Format = P.Format;
        COptions.Image = &Image;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
        EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        compileAndRun(Source + ReturnsSeven);
      }
  }
}

TEST(OwnInteriorCall, ClosedRecursionCannotRepairUnknownOrUnbalancedEffects) {
  for (const std::vector<uint8_t> &Effect : {
           std::vector<uint8_t>{0x48, 0x89, 0x04, 0x24}, // replace this PC
           std::vector<uint8_t>{0x48, 0x89, 0x01},       // arbitrary write
           std::vector<uint8_t>{0xFF, 0xD0},             // unknown call
           std::vector<uint8_t>{0x48, 0x83, 0xEC, 8},    // unbalanced frame
       }) {
    std::vector<uint8_t> Code = {0xB9, 5, 0, 0, 0, 0xE8, 6, 0, 0, 0, 0xC3};
    Code.resize(0x10, 0xCC);
    Code.insert(Code.end(), {0xE8, 0x0B, 0, 0, 0, 0xC3});
    Code.resize(0x20, 0xCC);
    Code.insert(Code.end(), Effect.begin(), Effect.end());
    Code.insert(Code.end(), {0x83, 0xE9, 1, 0x75});
    Code.push_back(uint8_t(0x10 - (Code.size() + 1)));
    Code.push_back(0xC3);
    const auto Result = lift(Code);
    EXPECT_FALSE(Result.UnprovenReturns.empty());
  }
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

TEST(OwnInteriorCall, SystemVCallsForTheirReturnAddressPushAndJump) {
  // The same body as above under a DWARF FDE.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100A
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001010
                         0xcc,                         // int3
                         0x48, 0x83, 0xc4, 0x10,       // add  rsp, 10h
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3},                        // ret
                        SysV64);
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_EQ(L.Source.find("sub_14000100A"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("sub_140001010"), std::string::npos) << L.Source;
  compileAndRun(L.Source + ReturnsSeven);
}

TEST(OwnInteriorCall, SystemVSubroutineThatReturnsStaysACall) {
  std::vector<uint8_t> Code = {0xe8, 0x0b, 0x00, 0x00, 0x00, // call 0x140001010
                               0x83, 0xc0, 0x01,             // add  eax, 1
                               0xc3};                        // ret
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0xb8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
                           0xc3});                       // ret
  const Lifted L = lift(std::move(Code), SysV64);
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_NE(L.Source.find("sub_140001010()"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, SystemVReturnFromASwitchedStackIsRefused) {
  // The target loads the stack pointer from a register and returns on that
  // stack: nothing shows its return pops the address the call pushed, so the
  // call is not the call it looks like.
  const Lifted L = lift({0xe8, 0x02, 0x00, 0x00, 0x00, // call 0x140001007
                         0xeb, 0x04,                   // jmp  0x14000100B
                         0x48, 0x89, 0xd4,             // mov  rsp, rdx
                         0xc3,                         // ret
                         0xc3},                        // ret
                        SysV64);
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x14000100A});
}

TEST(OwnInteriorCall, I386CallsForTheirReturnAddressPushAndJump) {
  // Four-byte return addresses: `add esp, 8` discards both.
  const Lifted L = lift({0x55,                         // push ebp
                         0x89, 0xe5,                   // mov  ebp, esp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x8049009
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x804900F
                         0xcc,                         // int3
                         0x83, 0xc4, 0x08,             // add  esp, 8
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  ebp
                         0xc3},                        // ret
                        I386);
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_TRUE(L.UnprovenReturns.empty());
  EXPECT_EQ(L.Source.find("sub_8049009"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("sub_804900F"), std::string::npos) << L.Source;
  EXPECT_NE(L.Source.find("7"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, I386ReturnBelowTheEntryStackPointerIsRefused) {
  // Discarding four bytes leaves the other return address for `pop ebp`.
  const Lifted L = lift({0x55,                         // push ebp
                         0x89, 0xe5,                   // mov  ebp, esp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x8049009
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x804900F
                         0xcc,                         // int3
                         0x83, 0xc4, 0x04,             // add  esp, 4
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  ebp
                         0xc3},                        // ret
                        I386);
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x8049018});
}

TEST(OwnInteriorCall, RecoveredRegistrationRangeBoundsNothing) {
  // An `_except_handler3` range is NeverD's reconstruction, not the image's
  // declaration, so the call stays a call and nothing is refused.
  const Lifted L = lift({0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x401006
                         0xcc,                         // int3
                         0x8b, 0x21,                   // mov  esp, [ecx]
                         0xc3},                        // ret
                        I386Registration);
  EXPECT_TRUE(L.UnprovenReturns.empty());
  EXPECT_NE(L.Source.find("sub_401006"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, OnlyDeclaredSingleFunctionRecordsBoundTheBody) {
  for (ExceptionEncoding E :
       {ExceptionEncoding::X64UnwindV1, ExceptionEncoding::X64UnwindV2,
        ExceptionEncoding::X64UnwindV3, ExceptionEncoding::ARM64Unpacked,
        ExceptionEncoding::DwarfFDE, ExceptionEncoding::GoFuncTable})
    EXPECT_TRUE(exceptionEncodingBoundsOneFunction(E))
        << getExceptionEncodingName(E);
  for (ExceptionEncoding E :
       {ExceptionEncoding::Unknown, ExceptionEncoding::CompactUnwind,
        ExceptionEncoding::X86ScopeTableEH3,
        ExceptionEncoding::X86ScopeTableEH4, ExceptionEncoding::X86CxxFuncInfo,
        ExceptionEncoding::DelphiX86Chain, ExceptionEncoding::ARMEHABIInline,
        ExceptionEncoding::ARMEHABIGeneric})
    EXPECT_FALSE(exceptionEncodingBoundsOneFunction(E))
        << getExceptionEncodingName(E);
}

constexpr Platform Darwin64 = {Arch::X64, BinaryFormat::MachO,
                               ExceptionEncoding::DwarfFDE, Entry};

void expectSourceRefused(const BinaryImage &Img) {
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(::testing::Message() << LLVM << '/' << NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Img.Entry};
      Options.SourceProjection = true;
      Options.LiftMode = LLVM;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Img, Context, Options);
      ASSERT_EQ(Result.FunctionAudits.size(), 1u);
      const auto &Audit = Result.FunctionAudits.front();
      EXPECT_EQ(Audit.Disposition,
                PipelineFunctionDisposition::RejectedIncomplete);
      EXPECT_FALSE(Audit.UnprovenReturns.empty());
      EXPECT_FALSE(Audit.HasLLVMDefinition);
      EXPECT_TRUE(Result.HighFuncs.empty());
    }
}

TEST(OwnInteriorCall, IndirectTailMustRestoreTheParentsStack) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386})
    for (uint8_t Restore : {0, 32, 40}) {
      const bool Restored = Restore == 32;
      SCOPED_TRACE(::testing::Message()
                   << int(P.TheArch) << '/' << int(P.Format) << '/'
                   << int(Restore));
      std::vector<uint8_t> Code;
      if (P.TheArch == Arch::X64)
        Code.push_back(0x48);
      // Allocate a frame, call the next label, then discard that internal PC.
      Code.insert(Code.end(), {0x83, 0xec, 32, 0xe8, 1, 0, 0, 0, 0xcc, 0x58});
      if (Restore) {
        if (P.TheArch == Arch::X64)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x83, 0xc4, Restore});
      }
      // A callback can receive the parent's PC only after its frame is gone.
      // The CALL + RETURN used to represent this JMP must not lose that fact.
      const va_t Tail = P.Entry + Code.size();
      Code.insert(Code.end(), {0xff, 0xe1}); // jmp ecx/rcx
      auto Img = makeImage(std::move(Code), P);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      CFGBuilder Builder;
      const auto Low = Builder.build(Img, Dec, P.Entry, "tail_frame");
      EXPECT_EQ(Low.hasCompleteLiftCoverage(), Restored);
      EXPECT_EQ(Low.UnprovenReturnAddresses,
                Restored ? std::vector<va_t>{} : std::vector<va_t>{Tail});
      if (!Restored)
        expectSourceRefused(Img);
    }
}

TEST(OwnInteriorCall, DiscardedCallMustRetainTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386})
    for (bool Tail : {false, true})
      for (bool OverwriteParent : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(P.TheArch) << '/' << int(P.Format) << '/' << Tail
                     << '/' << OverwriteParent);
        const uint8_t Width = P.TheArch == Arch::X64 ? 8 : 4;
        // Overwriting the discarded internal PC is harmless; overwriting
        // the parent's PC cannot become an ordinary return or callback.
        std::vector<uint8_t> Code{
            0xe8, 1,    0,    0,    0,
            0xcc, 0xc7, 0x44, 0x24, uint8_t(OverwriteParent ? Width : 0),
            0,    0,    0,    0};
        if (P.TheArch == Arch::X64)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x83, 0xc4, Width});
        if (Tail)
          Code.insert(Code.end(), {0xff, 0xe1}); // jmp ecx/rcx
        else
          Code.insert(Code.end(), {0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret
        auto Img = makeImage(std::move(Code), P);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        CFGBuilder Builder;
        const auto Low = Builder.build(Img, Dec, P.Entry, "parent_pc");
        EXPECT_EQ(Low.hasCompleteLiftCoverage(), !OverwriteParent);
        EXPECT_EQ(Low.UnprovenReturnAddresses.empty(), !OverwriteParent);
        if (OverwriteParent)
          expectSourceRefused(Img);
      }
}

Lifted liftOwnReturnBody(const Platform &P, llvm::ArrayRef<uint8_t> Body) {
  std::vector<uint8_t> Code = {0xe8, 0x0b, 0, 0, 0, 0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), Body.begin(), Body.end());
  return lift(std::move(Code), P);
}

TEST(OwnInteriorCall, OverwrittenReturnSlotCannotProveAnOrdinaryReturn) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // mov dword ptr [e/rsp], 0; ret. On x64 this destroys half of the PC.
    const auto L = liftOwnReturnBody(P, {0xc7, 0x04, 0x24, 0, 0, 0, 0, 0xc3});
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

void checkRecoveredTailSource(bool DiscardFrame) {
  for (const auto &P : {Win64, SysV64, Darwin64})
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(P.Format) << '/' << LLVM << '/' << NoOpt);
        // Both a replaced-PC RET and a JMP after discarding a frame must
        // pass the original parent's stack and the same callback arguments.
        const uint8_t Input = P.Format == BinaryFormat::COFF ? 0xc8 : 0xf8;
        std::vector<uint8_t> Code{0x48, 0x89, Input};
        if (DiscardFrame)
          Code.insert(Code.end(), {0x48, 0x83, 0xec, 32}); // sub rsp,32
        Code.insert(Code.end(), {0xe8, 1, 0, 0, 0, 0xcc});
        const std::vector<uint8_t> Setup =
            P.Format == BinaryFormat::COFF
                ? std::vector<uint8_t>{0xb9, 17,   0,    0,    0,    0x48,
                                       0x83, 0xc2, 1,    0x49, 0x83, 0xc0,
                                       2,    0x49, 0x83, 0xc1, 3}
                : std::vector<uint8_t>{0xbf, 17,   0,    0,    0,    0x48,
                                       0x83, 0xc6, 1,    0x48, 0x83, 0xc2,
                                       2,    0x48, 0x83, 0xc1, 3};
        Code.insert(Code.end(), Setup.begin(), Setup.end());
        if (DiscardFrame)
          Code.insert(Code.end(), {0x48, 0x83, 0xc4, 40, 0xff,
                                   0xe0}); // add rsp,40; jmp rax
        else
          Code.insert(Code.end(),
                      {0x48, 0x89, 0x04, 0x24, 0xc3}); // mov [rsp],rax; ret
        auto Img = makeImage(std::move(Code), P);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Img.Entry};
        Options.SourceProjection = true;
        Options.LiftMode = LLVM;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Img, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_FALSE(Result.MedFuncs.empty());
        for (const auto &Audit : Result.FunctionAudits)
          EXPECT_TRUE(Audit.UnprovenReturns.empty());
        std::string Source;
        llvm::raw_string_ostream Stream(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Img.Arch;
        COptions.Format = Img.Format;
        COptions.Image = &Img;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Stream, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, COptions));
        EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
        const char *CallbackValue =
            DiscardFrame ? "#define CALLBACK_VALUE ((void *)&callback)\n"
                         : "#define CALLBACK_VALUE ((uintptr_t)&callback)\n";
        compileAndRun(Source + CallbackValue + R"C(
static unsigned calls;
static uint64_t callback(uint64_t first, uint64_t a, uint64_t b, uint64_t c) {
  ++calls;
  return first == 17 ? ((a + 17) ^ b) - c : 0;
}
int main(void) {
  for (unsigned i = 0; i != 256; ++i) {
    uint64_t expected = ((i + 18) ^ (3 * i + 2)) - (uint64_t)(5 * i + 3);
    if ((uint64_t)sub_140001000(CALLBACK_VALUE, i, 3*i, 5*i) != expected)
      return 1;
  }
  return calls != 256;
}
)C");
      }
}

TEST(OwnInteriorCall, ReplacedPCIsAnIndirectTailTransferOnBothSourceRoutes) {
  checkRecoveredTailSource(false);
}

TEST(OwnInteriorCall, RestoredParentStackReachesTheCallbackOnBothSourceRoutes) {
  checkRecoveredTailSource(true);
}

TEST(OwnInteriorCall, ReplacedPCDoesNotRepairParentCorruptionOrAnExtraFrame) {
  for (const auto &P : {Win64, SysV64, Darwin64}) {
    // A complete replacement of the internal PC still cannot authorize a
    // tail call when the parent PC was overwritten, or when its stack frame
    // is not restored by this pop.
    const auto Corrupt = liftOwnReturnBody(
        P, {0x48, 0x89, 0x4c, 0x24, 8, 0x48, 0x89, 0x04, 0x24, 0xc3});
    EXPECT_TRUE(Corrupt.Source.empty()) << Corrupt.Source;
    EXPECT_FALSE(Corrupt.UnprovenReturns.empty());
    const auto Allocated = liftOwnReturnBody(
        P, {0x48, 0x83, 0xec, 8, 0x48, 0x89, 0x04, 0x24, 0xc3});
    EXPECT_TRUE(Allocated.Source.empty()) << Allocated.Source;
    EXPECT_FALSE(Allocated.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, UnknownStoreMayOverwriteTheActiveReturnSlot) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // mov dword ptr [ecx/rcx], 0; ret. The pointer may name the active PC.
    const auto L = liftOwnReturnBody(P, {0xc7, 0x01, 0, 0, 0, 0, 0xc3});
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, StackAliasesAndAtomicWritesCannotDestroyTheReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // mov eax/rax, esp/rsp; mov dword [eax/rax], 0; ret.
    std::vector<uint8_t> Alias;
    if (P.TheArch == Arch::X64)
      Alias.push_back(0x48);
    Alias.insert(Alias.end(), {0x89, 0xe0, 0xc7, 0x00, 0, 0, 0, 0, 0xc3});
    const auto A = liftOwnReturnBody(P, Alias);
    EXPECT_TRUE(A.Source.empty()) << A.Source;
    EXPECT_FALSE(A.UnprovenReturns.empty());
    // xchg [e/rsp], eax; ret.
    const auto B = liftOwnReturnBody(P, {0x87, 0x04, 0x24, 0xc3});
    EXPECT_TRUE(B.Source.empty()) << B.Source;
    EXPECT_FALSE(B.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, PartialOverlapIsCheckedAfterStackAllocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    for (uint8_t Offset : {uint8_t{0}, uint8_t{6}}) {
      SCOPED_TRACE(int(Offset));
      // Allocate eight bytes; the store at +6 straddles the PC, while the
      // one at +0 is wholly in the callee's own allocation.
      std::vector<uint8_t> Body;
      if (P.TheArch == Arch::X64)
        Body.push_back(0x48);
      Body.insert(Body.end(),
                  {0x83, 0xec, 8, 0xc7, 0x44, 0x24, Offset, 0, 0, 0, 0});
      if (P.TheArch == Arch::X64)
        Body.push_back(0x48);
      Body.insert(Body.end(), {0x83, 0xc4, 8, 0xc3});
      const auto L = liftOwnReturnBody(P, Body);
      EXPECT_EQ(L.UnprovenReturns.empty(), Offset == 0) << L.Source;
      EXPECT_EQ(L.Source.empty(), Offset != 0) << L.Source;
    }
  }
}

TEST(OwnInteriorCall, OpaqueCalleeDoesNotProveItsCallersReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // call ecx/rcx; ret. A balanced SP says nothing about the callee's writes.
    const auto L = liftOwnReturnBody(P, {0xff, 0xd1, 0xc3});
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, ClobbersOnTerminatingPathsDoNotInvalidateOtherReturns) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // test eax,eax; jz ret; mov dword [ecx/rcx],0; ud2; ret.
    const auto L = liftOwnReturnBody(
        P, {0x85, 0xc0, 0x74, 8, 0xc7, 0x01, 0, 0, 0, 0, 0x0f, 0x0b, 0xc3});
    EXPECT_TRUE(L.UnprovenReturns.empty());
    EXPECT_FALSE(L.Source.empty());
  }
}

TEST(OwnInteriorCall, DiscardedReturnSlotsNeedNotRetainTheirValue) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    std::vector<uint8_t> Body = {0xc7, 0x04, 0x24, 0, 0, 0, 0};
    if (P.TheArch == Arch::X64)
      Body.push_back(0x48);
    Body.insert(Body.end(),
                {0x83, 0xc4, uint8_t(P.TheArch == Arch::X64 ? 8 : 4), 0xb8, 7,
                 0, 0, 0, 0xc3});
    const auto L = liftOwnReturnBody(P, Body);
    ASSERT_TRUE(L.Unsupported.empty());
    ASSERT_TRUE(L.UnprovenReturns.empty());
    ASSERT_FALSE(L.Source.empty());
    if (P.TheArch == Arch::X64)
      compileAndRun(L.Source + ReturnsSeven);
  }
}

TEST(OwnInteriorCall, I386NegativeAddressDisplacementPreservesPrivateWrites) {
  // lea esp,[esp-8]; mov dword ptr [esp],0; add esp,8; ret. LowIR carries
  // the signed displacement in a 32-bit constant, then widens the address.
  const auto L =
      liftOwnReturnBody(I386, {0x8d, 0x64, 0x24, 0xf8, 0xc7, 0x04, 0x24, 0, 0,
                               0, 0, 0x83, 0xc4, 8, 0xc3});
  EXPECT_TRUE(L.Unsupported.empty());
  EXPECT_TRUE(L.UnprovenReturns.empty());
  EXPECT_FALSE(L.Source.empty());
}

std::vector<uint8_t> mixedReturnCode(const Platform &P) {
  std::vector<uint8_t> Code = {0xb8, 11,   0, 0,   0, // mov eax,11
                               0xe8, 0x16, 0, 0,   0, // call entry+0x20
                               0x83, 0xc0, 1, 0xc3};  // add eax,1; ret
  Code.resize(0x20, 0xcc);
  if (P.TheArch == Arch::X86)
    Code.insert(Code.end(), {0x83, 0x7c, 0x24, 8, 0}); // cmp [esp+8],0
  else
    Code.insert(Code.end(), {0x85, uint8_t(P.Format == BinaryFormat::COFF
                                               ? 0xc9     // test ecx,ecx
                                               : 0xff)}); // test edi,edi
  std::vector<uint8_t> Nonlocal;
  if (P.TheArch == Arch::X64)
    Nonlocal.push_back(0x48);
  Nonlocal.insert(Nonlocal.end(),
                  {0x83, 0xc4, uint8_t(P.TheArch == Arch::X64 ? 8 : 4), 0xb8, 7,
                   0, 0, 0, 0xc3});
  Code.insert(Code.end(), {0x74, uint8_t(Nonlocal.size())});
  Code.insert(Code.end(), Nonlocal.begin(), Nonlocal.end());
  Code.push_back(0xc3); // ordinary return consumes this call's PC
  return Code;
}

TEST(OwnInteriorCall, MixedExitsResumeOnlyTheMatchingInvocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    const auto Code = mixedReturnCode(P);
    const auto L = lift(Code, P);
    ASSERT_TRUE(L.Unsupported.empty());
    ASSERT_TRUE(L.UnprovenReturns.empty());
    ASSERT_FALSE(L.Source.empty());
    EXPECT_EQ(L.Source.find("unknown value"), std::string::npos) << L.Source;
    if (P.TheArch == Arch::X64)
      compileAndRun(L.Source + R"C(
int main(void) {
  return sub_140001000(0) != 12 || sub_140001000(1) != 7 ||
         sub_140001000(-1) != 7;
}
)C");
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt);
      auto Img = makeImage(Code, P);
      llvm::LLVMContext Ctx;
      PipelineOptions Opts;
      Opts.EmitDumpOutput = false;
      Opts.LiftMode = true;
      Opts.NoOpt = NoOpt;
      Opts.OnlyFunctionEntries.insert(P.Entry);
      auto Result = Pipeline().run(Img, Ctx, Opts);
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      ASSERT_EQ(Result.FunctionAudits.size(), 1u);
      EXPECT_TRUE(Result.FunctionAudits.front().HasLLVMDefinition);
    }
  }
}

TEST(OwnInteriorCall, MixedReturnStillRequiresAnUnmodifiedNearReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << int(P.TheArch) << '/'
                                        << int(P.Format) << '/' << Mutation);
      auto Code = mixedReturnCode(P);
      Code.pop_back();
      if (Mutation == 0)
        Code.insert(Code.end(), {0xc7, 0x04, 0x24, 0, 0, 0, 0, 0xc3});
      else if (Mutation == 1)
        Code.insert(Code.end(), {0xc7, 0x01, 0, 0, 0, 0, 0xc3});
      else
        Code.insert(Code.end(), {0xc2, 4, 0});
      const auto L = lift(std::move(Code), P);
      EXPECT_TRUE(L.Source.empty()) << L.Source;
      EXPECT_FALSE(L.UnprovenReturns.empty());
    }
  }
}

TEST(OwnInteriorCall, NonlocalExitMustRetainTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << int(P.TheArch) << '/'
                                        << int(P.Format) << '/' << Mutation);
      auto Code = mixedReturnCode(P);
      const size_t Body = P.TheArch == Arch::X64 ? 0x24 : 0x27;
      const unsigned Width = P.TheArch == Arch::X64 ? 8 : 4;
      if (Mutation == 0)
        Code[Body + (P.TheArch == Arch::X64 ? 3 : 2)] = 2 * Width;
      else {
        // The active internal PC and the parent's physical PC have different
        // ownership. A nonlocal return must not consume a changed parent PC.
        const std::vector<uint8_t> Write =
            Mutation == 2 ? std::vector<uint8_t>{0xc7, 0x01, 0, 0, 0, 0}
                          : std::vector<uint8_t>{
                                0xc7, 0x44, 0x24, uint8_t(Width), 0, 0, 0, 0};
        Code.insert(Code.begin() + Body, Write.begin(), Write.end());
        Code[Body - 1] += Write.size();
      }
      const auto L = lift(std::move(Code), P);
      EXPECT_TRUE(L.Source.empty()) << L.Source;
      EXPECT_FALSE(L.UnprovenReturns.empty());
    }
  }
}

TEST(OwnInteriorCall, NonlocalPrivateWritesDoNotAliasTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    const bool X64 = P.TheArch == Arch::X64;
    const unsigned Width = X64 ? 8 : 4;
    std::vector<uint8_t> Code;
    if (X64)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x83, 0xec, 16, 0xb8, 11, 0, 0, 0});
    const unsigned Continuation = Code.size() + 5;
    Code.insert(Code.end(),
                {0xe8, uint8_t(0x20 - Continuation), 0, 0, 0, 0x83, 0xc0, 1});
    if (X64)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x83, 0xc4, 16, 0xc3});
    Code.resize(0x20, 0xcc);
    const auto Single = mixedReturnCode(P);
    std::vector<uint8_t> Body(Single.begin() + 0x20, Single.end());
    const unsigned Nonlocal = X64 ? 4 : 7;
    if (!X64)
      Body[3] += 16; // the incoming parameter is above the parent allocation
    Body[Nonlocal + (X64 ? 3 : 2)] += 16;
    const std::vector<uint8_t> Write{0xc7, 0x44, 0x24, uint8_t(Width),
                                     0,    0,    0,    0};
    Body[Nonlocal - 1] += Write.size();
    Body.insert(Body.begin() + Nonlocal, Write.begin(), Write.end());
    Code.insert(Code.end(), Body.begin(), Body.end());
    const auto L = lift(std::move(Code), P);
    ASSERT_TRUE(L.Unsupported.empty());
    ASSERT_TRUE(L.UnprovenReturns.empty());
    ASSERT_FALSE(L.Source.empty());
    if (X64)
      compileAndRun(L.Source + R"C(
int main(void) { return sub_140001000(0) != 12 || sub_140001000(1) != 7; }
)C");
  }
}

TEST(OwnInteriorCall, MultipleCallSitesCannotShareOneMixedReturnCertificate) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    // Branch to either of two calls of the same target. Both continuations
    // are distinct even though their physical stack offsets agree.
    auto Code = mixedReturnCode(P);
    const std::vector<uint8_t> Prefix = {0x85, 0xc0, 0x74, 6,    0xe8, 0x17,
                                         0,    0,    0,    0xc3, 0xe8, 0x11,
                                         0,    0,    0,    0xc3};
    std::copy(Prefix.begin(), Prefix.end(), Code.begin());
    const auto L = lift(std::move(Code), P);
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, IndependentAndExceptionalRootsDoNotBorrowAnInvocation) {
  for (bool Exceptional : {false, true}) {
    SCOPED_TRACE(Exceptional);
    auto Code = mixedReturnCode(Win64);
    const auto Return = Win64.Entry + Code.size() - 1;
    auto Img = makeImage(std::move(Code), Win64);
    if (Exceptional) {
      auto &Own = Img.ExceptionMetadata.Functions.front();
      Own.SEH.emplace();
      Own.SEH->Scopes.push_back({{Win64.Entry + 5, Win64.Entry + 10},
                                 SEHScopeKind::CatchAll,
                                 1,
                                 Return,
                                 Return});
    }
    Img.ExceptionMetadata.rebuildIndex();
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const std::set<va_t> Roots{Return};
    if (!Exceptional)
      Builder.setCrossFunctionContinuationRoots(&Roots);
    const auto Low = Builder.build(Img, Dec, Win64.Entry, "mixed");
    if (!Exceptional)
      EXPECT_TRUE(Low.OrdinaryModuleAnalysisRoots.count(Return));
    EXPECT_FALSE(Low.UnprovenReturnAddresses.empty());
    EXPECT_FALSE(Low.hasCompleteLiftCoverage());
  }
}

TEST(OwnInteriorCall, NewlyDecodedContinuationMustRevalidateReturnOwnership) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    auto Code = mixedReturnCode(P);
    // The normal continuation reaches the same RET with the parent's SP.
    // That edge is decoded only after proposing the internal return. Its
    // independent invocation must invalidate the provisional certificate.
    Code[10] = 0xe9;
    Code[11] = uint8_t(Code.size() - 1 - 15);
    Code[12] = Code[13] = Code[14] = 0;
    const auto L = lift(std::move(Code), P);
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, RepeatedSingleSiteInvocationsRetainTheirStackBalance) {
  std::vector<uint8_t> Code = {
      0xb8, 11,   0,   0,    0,    // mov eax,11
      0xb9, 2,    0,   0,    0,    // mov ecx,2
      0xe8, 0x31, 0,   0,    0,    // call entry+0x40
      0x83, 0xc0, 1,   0xff, 0xc9, // add eax,1; dec ecx
      0x75, 0xf4, 0xc3};           // jnz call; ret
  Code.resize(0x40, 0xcc);
  const auto Single = mixedReturnCode(SysV64);
  Code.insert(Code.end(), Single.begin() + 0x20, Single.end());
  const auto L = lift(std::move(Code), SysV64);
  ASSERT_TRUE(L.Unsupported.empty());
  ASSERT_TRUE(L.UnprovenReturns.empty());
  ASSERT_FALSE(L.Source.empty());
  compileAndRun(L.Source + R"C(
int main(void) {
  return sub_140001000(0) != 13 || sub_140001000(1) != 7;
}
)C");
}

TEST(OwnInteriorCall, OpenIndirectExitCannotCertifyAMixedInvocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    auto Code = mixedReturnCode(P);
    const auto BodyBytes = Code.size() - 0x20;
    const std::vector<uint8_t> Prefix = {0x85, 0xd2, 0x75, uint8_t(BodyBytes)};
    Code.insert(Code.begin() + 0x20, Prefix.begin(), Prefix.end());
    Code.insert(Code.end(), {0xff, 0xe2}); // jmp edx/rdx
    const auto L = lift(std::move(Code), P);
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, LanguageRuntimeInvocationsNeedTheirOwnExitContract) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Img = makeImage(mixedReturnCode(SysV64), SysV64);
    auto &EH = Img.ExceptionMetadata.Functions.front();
    if (Mutation == 0)
      EH.Itanium.emplace();
    else if (Mutation == 1)
      EH.PersonalityVA = SysV64.Entry + 0x100;
    else if (Mutation == 2) {
      EH.Dwarf.emplace();
      EH.Dwarf->LSDAVA = SysV64.Entry + 0x100;
    } else
      EH.ParseStatus = Mutation == 3 ? ExceptionParseStatus::Partial
                                     : ExceptionParseStatus::Malformed;
    Img.ExceptionMetadata.rebuildIndex();
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const auto Low = Builder.build(Img, Dec, SysV64.Entry, "mixed");
    EXPECT_FALSE(Low.UnprovenReturnAddresses.empty());
    EXPECT_FALSE(Low.hasCompleteLiftCoverage());
  }
}

TEST(OwnInteriorCall, OrdinaryInternalReturnsRetainTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    std::vector<uint8_t> Code{0xe8, 0x1b, 0, 0, 0, 0xc3}; // call entry+32; ret
    Code.resize(0x20, 0xcc);
    // The callee's own PC remains intact, but this write changes the parent's
    // physical return address. A normal subroutine classification is unsafe.
    Code.insert(Code.end(),
                {0xc7, 0x44, 0x24, uint8_t(P.TheArch == Arch::X86 ? 4 : 8), 0,
                 0, 0, 0, 0xc3});
    auto Img = makeImage(std::move(Code), P);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const auto Low = Builder.build(Img, Dec, P.Entry, "parent_pc");
    EXPECT_FALSE(Low.UnprovenReturnAddresses.empty());
    EXPECT_FALSE(Low.hasCompleteLiftCoverage());
  }
}

TEST(OwnInteriorCall, OrdinaryInternalWritesCanUseTheParentsPrivateAllocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    std::vector<uint8_t> Code;
    if (P.TheArch == Arch::X64)
      Code.push_back(0x48);
    Code.insert(Code.end(),
                {0x83, 0xec, 16, 0xe8,
                 uint8_t(P.TheArch == Arch::X86 ? 0x18 : 0x17), 0, 0, 0});
    if (P.TheArch == Arch::X64)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x83, 0xc4, 16, 0xc3});
    Code.resize(0x20, 0xcc);
    // The same displacement now names allocated parent storage. Refusing
    // every write above the callee's PC would lose this valid ordinary call.
    Code.insert(Code.end(),
                {0xc7, 0x44, 0x24, uint8_t(P.TheArch == Arch::X86 ? 4 : 8), 7,
                 0, 0, 0, 0xc3});
    auto Img = makeImage(std::move(Code), P);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const auto Low = Builder.build(Img, Dec, P.Entry, "parent_private");
    EXPECT_TRUE(Low.UnprovenReturnAddresses.empty());
    EXPECT_TRUE(Low.hasCompleteLiftCoverage());
    EXPECT_TRUE(llvm::any_of(Low.Blocks, [&](const LowBlock &Block) {
      return llvm::any_of(Block.Ops, [&](const LowOp &Op) {
        return Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
               Op.Inputs[0].isConst() && Op.Inputs[0].Offset == P.Entry + 0x20;
      });
    }));
  }
}

} // namespace
