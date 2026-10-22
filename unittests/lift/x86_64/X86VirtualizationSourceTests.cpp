//===- X86VirtualizationSourceTests.cpp - VMX/SVM source tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the shared pipeline and both source routes across x86/x64 native
/// formats. These synthetic fixtures never execute privileged host operations.
//===----------------------------------------------------------------------===//

#include "X86VirtualizationFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::test;

namespace {
constexpr va_t Entry = 0x1000;

BinaryImage image(Arch A, BinaryFormat Format, std::vector<uint8_t> Bytes) {
  BinaryImage I;
  I.Arch = A;
  I.Format = Format;
  I.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  I.Base = I.Entry = Entry;
  Segment Text;
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Text.Size = Text.FileSz = Text.Data.size();
  auto FunctionSymbol = Symbol::makeFunc(Entry, Text.Size);
  FunctionSymbol.Name = "virtualization_test";
  I.Symbols.push_back(std::move(FunctionSymbol));
  I.Segments.push_back(std::move(Text));
  return I;
}

bool anchored(const CSourceMap &Map, va_t Address, llvm::StringRef Source,
              llvm::StringRef Mnemonic) {
  return std::any_of(
      Map.Anchors.begin(), Map.Anchors.end(), [&](const auto &A) {
        return A.Function == Entry && A.Span.End <= Source.size() &&
               Source.slice(A.Span.Begin, A.Span.End).contains(Mnemonic) &&
               std::any_of(A.Occurrences.begin(), A.Occurrences.end(),
                           [&](const auto &O) { return O.Address == Address; });
      });
}

void sourceRoutes(BinaryImage &Image, bool NoOpt, va_t Address,
                  llvm::StringRef Mnemonic, std::string *HighOutput = nullptr,
                  std::string *LLVMOutput = nullptr,
                  llvm::StringRef HighMnemonic = {},
                  llvm::StringRef LLVMMnemonic = {}) {
  if (HighMnemonic.empty())
    HighMnemonic = Mnemonic;
  if (LLVMMnemonic.empty())
    LLVMMnemonic = Mnemonic;
  PipelineOptions O;
  O.EmitDumpOutput = false;
  O.NoOpt = NoOpt;
  O.OnlyFunctionEntries = {Entry};
  llvm::LLVMContext Context;
  auto R = Pipeline().run(Image, Context, O);
  ASSERT_TRUE(R.Success) << R.Error;
  ASSERT_EQ(R.HighFuncs.size(), 1u);
  ASSERT_EQ(R.MedFuncs.size(), 1u);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  Options.Image = &Image;
  CSourceMap HighMap;
  HighMap.HighSources = &R.HighSources;
  Options.SourceMap = &HighMap;
  std::string High;
  llvm::raw_string_ostream HighOS(High);
  ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HighOS, Options));
  EXPECT_TRUE(anchored(HighMap, Address, High, HighMnemonic)) << High;
  if (Image.Format == BinaryFormat::COFF && Mnemonic == "cpuid") {
    EXPECT_NE(High.find("__cpuidex("), std::string::npos) << High;
    EXPECT_NE(High.find("_ReadWriteBarrier();"), std::string::npos) << High;
  }

  MedLLVMEmitter Emitter;
  LLVMSourceMap Sources;
  Emitter.setSourceMap(&Sources);
  auto Module = Emitter.emit(R.MedFuncs, Context, "virtualization", Image.Arch,
                             {}, &Image, Image.Format);
  ASSERT_NE(Module, nullptr);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  if (NoOpt)
    Pipeline::promoteScaffoldingAllocas(*Module);
  else
    (void)Pipeline::optimizeOrPromoteModule(*Module, &Sources);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  CSourceMap LLVMMap;
  LLVMMap.LLVMSources = &Sources;
  Options.SourceMap = &LLVMMap;
  std::string LLVM;
  llvm::raw_string_ostream LLVMOS(LLVM);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, LLVMOS, Options));
  EXPECT_TRUE(anchored(LLVMMap, Address, LLVM, LLVMMnemonic)) << LLVM;
  if (HighOutput)
    *HighOutput = High;
  if (LLVMOutput)
    *LLVMOutput = LLVM;
}

std::vector<uint8_t> function(Arch A, const std::vector<uint8_t> &Instruction,
                              size_t &InstructionOffset) {
  // Own enough initialized frame storage even for the 16-byte invalidation
  // descriptors. Distinct constants keep field/value operand ordering visible.
  std::vector<uint8_t> Bytes;
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0x83, 0xec, 0x20});
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0xc7, 0x04, 0x24, 0x55, 0, 0, 0});
  Bytes.insert(Bytes.end(), {0xb8, 0x56, 0x34, 0, 0, 0xb9, 0x34, 0x12, 0, 0,
                             0xba, 0x98, 0x76, 0, 0});
  InstructionOffset = Bytes.size();
  Bytes.insert(Bytes.end(), Instruction.begin(), Instruction.end());
  // Consume both flags before any arithmetic changes them.
  Bytes.insert(Bytes.end(),
               {0x0f, 0x92, 0xc0, 0x0f, 0x94, 0xc1, 0x0f, 0xb6, 0xc0, 0x0f,
                0xb6, 0xc9, 0xc1, 0xe1, 0x08, 0x09, 0xc8});
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0x83, 0xc4, 0x20, 0xc3});
  return Bytes;
}
} // namespace

TEST(X86VirtualizationSource, EveryOwnedInstructionSurvivesBothSourceRoutes) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &Encoding : Encodings) {
          const auto C = x86VirtualizationContract(Encoding.Id);
          SCOPED_TRACE(C.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Bytes = function(A, Encoding.Bytes, Offset);
          auto Image = image(A, Format, std::move(Bytes));
          sourceRoutes(Image, NoOpt, Entry + Offset, C.Mnemonic);
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86VirtualizationSource,
     CpuidAndSiblingIntrinsicsRetainTheirEffectAnchors) {
  struct Encoding {
    std::vector<uint8_t> Bytes;
    const char *Mnemonic;
    const char *CName = nullptr;
  };
  for (const auto &E : std::vector<Encoding>{{{0x0f, 0xa2}, "cpuid"},
                                             {{0x0f, 0x01, 0xd0}, "xgetbv"},
                                             {{0x0f, 0x31}, "rdtsc"},
                                             {{0x0f, 0x01, 0xf9}, "rdtscp"},
                                             {{0x0f, 0xae, 0xe8}, "lfence"},
                                             {{0x0f, 0xae, 0xf0}, "mfence"},
                                             {{0x0f, 0xae, 0xf8}, "sfence"},
                                             {{0xfa}, "cli", "_disable"},
                                             {{0xfb}, "sti", "_enable"}}) {
    SCOPED_TRACE(E.Mnemonic);
    for (const auto A : {Arch::X86, Arch::X64})
      for (const auto Format :
           {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
        for (bool NoOpt : {false, true}) {
          // All query outputs are dead. The call/fence remains an ordered
          // effect.
          std::vector<uint8_t> Bytes = {0x53, 0xb8, 0x07, 0, 0, 0,
                                        0xb9, 0x01, 0,    0, 0};
          const auto Offset = Bytes.size();
          Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
          Bytes.insert(Bytes.end(), {0x31, 0xc0, 0x5b, 0xc3});
          auto Image = image(A, Format, std::move(Bytes));
          sourceRoutes(
              Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr, nullptr,
              E.CName && Format == BinaryFormat::COFF ? E.CName : E.Mnemonic,
              E.CName ? E.CName : E.Mnemonic);
          if (::testing::Test::HasFailure())
            return;
        }
  }
}

TEST(X86VirtualizationSource, VmreadMemoryInvalidatesThePriorStackValue) {
  for (bool NoOpt : {false, true}) {
    auto Image =
        image(Arch::X64, BinaryFormat::ELF,
              {0x48, 0x83, 0xec, 0x08, 0x48, 0xc7, 0x04, 0x24, 0x55, 0,
               0,    0,    0xb9, 0x34, 0x12, 0,    0,    0x0f, 0x78, 0x0c,
               0x24, 0x48, 0x8b, 0x04, 0x24, 0x48, 0x83, 0xc4, 0x08, 0xc3});
    PipelineOptions O;
    O.NoOpt = NoOpt;
    O.EmitDumpOutput = false;
    O.OnlyFunctionEntries = {Entry};
    llvm::LLVMContext Context;
    auto R = Pipeline().run(Image, Context, O);
    ASSERT_TRUE(R.Success) << R.Error;
    MedLLVMEmitter Emitter;
    auto Module = Emitter.emit(R.MedFuncs, Context, "vmread_memory", Image.Arch,
                               {}, &Image, Image.Format);
    ASSERT_NE(Module, nullptr);
    if (NoOpt)
      Pipeline::promoteScaffoldingAllocas(*Module);
    else
      (void)Pipeline::optimizeOrPromoteModule(*Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    bool LoadedAfterRead = false;
    for (const auto &F : *Module)
      for (const auto &B : F) {
        bool Read = false;
        for (const auto &I : B) {
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
            if (const auto *Asm =
                    llvm::dyn_cast<llvm::InlineAsm>(Call->getCalledOperand()))
              Read |=
                  llvm::StringRef(Asm->getAsmString()).starts_with("vmread");
          LoadedAfterRead |= Read && llvm::isa<llvm::LoadInst>(I) &&
                             I.getType()->isIntegerTy(64);
        }
      }
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    Module->print(OS, nullptr);
    EXPECT_TRUE(LoadedAfterRead) << IR;
  }
}

TEST(X86VirtualizationSource, PhysicalRegistersKeepTheirArchitecture) {
  MedOp Op;
  Op.NumInputs = 1;
  Op.Inputs[0].Kind = MedVar::Const;
  Op.Inputs[0].ConstVal = static_cast<uint16_t>(Intrinsic::VmxOff);
  Op.Inputs[0].Size = 2;
  Op.Output.Kind = MedVar::Reg;
  Op.Output.Id = 0;
  Op.Output.Size = 2;
  Op.Output.TheArch = Arch::X64;
  EXPECT_FALSE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxOff, x86VirtualizationMedShape(Op, Arch::X86)));
  Op.Output.TheArch = Arch::X86;
  EXPECT_TRUE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxOff, x86VirtualizationMedShape(Op, Arch::X86)));
  // A temporary's default TheArch does not establish physical ownership.
  Op.Output.Kind = MedVar::Temp;
  Op.Output.TheArch = Arch::X64;
  EXPECT_TRUE(x86VirtualizationShapeIsValid(
      Intrinsic::VmxOff, x86VirtualizationMedShape(Op, Arch::X86)));
}

TEST(X86VirtualizationSource, CpuidCExecutesOnceWithSubleafAndUnsignedOutputs) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-cpuid-source", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  for (const auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool NoOpt : {false, true}) {
      // Store every architectural result into a 64-bit cell. High-bit words
      // expose accidental sign extension of MSVC's signed int output array.
      std::vector<uint8_t> Bytes = {
          0x53, 0x49,
          0x89, Format == BinaryFormat::COFF ? uint8_t{0xc9} : uint8_t{0xf9},
          0xb8, 7,
          0,    0,
          0,    0xb9,
          1,    0,
          0,    0,
          0x0f, 0xa2,
          0x49, 0x89,
          0x01, 0x49,
          0x89, 0x59,
          0x08, 0x49,
          0x89, 0x49,
          0x10, 0x49,
          0x89, 0x51,
          0x18, 0x31,
          0xc0, 0x5b,
          0xc3};
      auto Image = image(Arch::X64, Format, std::move(Bytes));
      std::string High;
      sourceRoutes(Image, NoOpt, Entry + 14, "cpuid", &High);
      if (::testing::Test::HasFailure())
        return;
      std::string Program = R"C(
#include <stdint.h>
static unsigned calls, leaf_seen, subleaf_seen;
static const uint32_t words[4] = {0x80000000u, 0xfedcba98u, 0x87654321u, 0xffffffffu};
static void query(unsigned leaf, unsigned subleaf, unsigned *a, unsigned *b,
                  unsigned *c, unsigned *d) {
  ++calls; leaf_seen = leaf; subleaf_seen = subleaf;
  *a = words[0]; *b = words[1]; *c = words[2]; *d = words[3];
}
#define __cpuid(l,a,b,c,d) query(l,0,&(a),&(b),&(c),&(d))
#define __cpuid_count(l,s,a,b,c,d) query(l,s,&(a),&(b),&(c),&(d))
#define __cpuidex(o,l,s) query(l,s,(unsigned *)&o[0],(unsigned *)&o[1],(unsigned *)&o[2],(unsigned *)&o[3])
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
)C";
      // Only the intrinsic/header environment is replaced. Keep the emitted
      // declaration and body unchanged, including all four output transports.
      llvm::SmallVector<llvm::StringRef> Lines;
      llvm::StringRef(High).split(Lines, '\n');
      for (const auto Line : Lines)
        if (!Line.starts_with("#include"))
          Program += Line.str() + '\n';
      Program += R"C(
int main(void) {
  uint64_t out[4] = {0, 0, 0, 0};
  (void)virtualization_test(out);
  if (calls != 1 || leaf_seen != 7 || subleaf_seen != 1) return 1;
  for (unsigned i = 0; i < 4; ++i) if (out[i] != words[i]) return 2 + i;
  return 0;
}
)C";
      llvm::SmallString<128> Input(Directory), Executable(Directory),
          ErrorFile(Directory);
      llvm::sys::path::append(Input, "cpuid.c");
      llvm::sys::path::append(Executable, "cpuid.exe");
      llvm::sys::path::append(ErrorFile, "clang.err");
      std::error_code Error;
      llvm::raw_fd_ostream OS(Input, Error);
      ASSERT_FALSE(Error);
      OS << Program;
      OS.close();
      ASSERT_FALSE(OS.has_error());
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Optimization);
        const std::vector<llvm::StringRef> Arguments = {
            Compiler, "-std=gnu11", "-fno-strict-aliasing", Optimization, Input,
            "-o",     Executable};
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, ErrorFile.str()};
        const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                     std::nullopt, Redirects);
        const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
        ASSERT_EQ(Status, 0)
            << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "")
            << Program;
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0)
            << High;
      }
    }
}

TEST(X86VirtualizationSource,
     GeneratedAssemblyCompilesForBothWordSizesAtO0AndO2) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-vmx-source", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &Encoding : Encodings) {
      const auto C = x86VirtualizationContract(Encoding.Id);
      SCOPED_TRACE(C.Mnemonic);
      size_t Offset;
      auto Image =
          image(A, BinaryFormat::ELF, function(A, Encoding.Bytes, Offset));
      std::string High, LLVM;
      sourceRoutes(Image, false, Entry + Offset, C.Mnemonic, &High, &LLVM);
      if (::testing::Test::HasFailure())
        return;
      for (auto Source : {High, LLVM}) {
        // Freestanding Clang supplies stdint and the instruction headers for
        // the requested x86 target on every host. Avoid a host's 32-bit libc
        // dependency for the generated byte-copy declarations.
        const std::string Include = "#include <string.h>";
        if (const auto At = Source.find(Include); At != std::string::npos)
          Source.replace(
              At, Include.size(),
              "extern void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
              "extern void *memset(void *, int, __SIZE_TYPE__);");
        llvm::SmallString<128> Input(Directory), Output(Directory),
            ErrorFile(Directory);
        llvm::sys::path::append(Input, "source.c");
        llvm::sys::path::append(Output, "source.o");
        llvm::sys::path::append(ErrorFile, "clang.err");
        std::error_code Error;
        llvm::raw_fd_ostream OS(Input, Error);
        ASSERT_FALSE(Error);
        OS << Source;
        OS.close();
        ASSERT_FALSE(OS.has_error());
        for (const char *Optimization : {"-O0", "-O2"}) {
          const std::vector<llvm::StringRef> Arguments = {
              Compiler,
              A == Arch::X86 ? "--target=i386-unknown-linux-gnu"
                             : "--target=x86_64-unknown-linux-gnu",
              "-std=gnu11",
              "-ffreestanding",
              "-fno-strict-aliasing",
              "-Werror=implicit-function-declaration",
              Optimization,
              "-c",
              Input,
              "-o",
              Output};
          const std::optional<llvm::StringRef> Redirects[] = {
              std::nullopt, std::nullopt, ErrorFile.str()};
          const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                       std::nullopt, Redirects);
          const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
          ASSERT_EQ(Status, 0)
              << Optimization << '\n'
              << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "")
              << Source;
        }
      }
    }
}
