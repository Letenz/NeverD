//===- X86VirtualizationSourceTests.cpp - VMX/SVM source tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the shared pipeline and both source routes across x86/x64 native
/// formats. Privileged success is not assumed; isolated system-memory probes
/// compare faults with the original bytes on the same host.
//===----------------------------------------------------------------------===//

#include "X86SHA512Reference.h"
#include "X86SystemStateFixtures.h"
#include "X86VirtualizationFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86SystemStateAsm.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
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
                  llvm::StringRef LLVMMnemonic = {},
                  std::string *NativeOutput = nullptr,
                  std::vector<uint8_t> *NativeObject = nullptr) {
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
  if (!Mnemonic.empty())
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
  if (!Mnemonic.empty())
    EXPECT_TRUE(anchored(LLVMMap, Address, LLVM, LLVMMnemonic)) << LLVM;
  if (NativeOutput) {
    llvm::raw_string_ostream OS(*NativeOutput);
    Module->print(OS, nullptr);
  }
  if (NativeObject) {
    auto Copy = llvm::CloneModule(*Module);
    auto Object = Codegen().compile(*Copy, Image.Arch, Image.Format);
    ASSERT_TRUE(Object.Success);
    ASSERT_FALSE(Object.ObjectData.empty());
    *NativeObject = std::move(Object.ObjectData);
  }
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

namespace {
std::string freestandingC(std::string Source) {
  // Cross-target checks use the real compiler intrinsic headers without
  // requiring a target CRT installation. Only the standard memory prototypes
  // are supplied; generated declarations, bodies and assembly stay intact.
  const std::string Include = "#include <string.h>";
  if (const auto At = Source.find(Include); At != std::string::npos)
    Source.replace(At, Include.size(),
                   "extern void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
                   "extern void *memset(void *, int, __SIZE_TYPE__);");
  return Source;
}

std::string faultDriver(const BinaryImage &Image) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  OS << R"C(
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern void virtualization_test(void);
extern void neverd_original(void);
static int result_fd;
static void fault(int signal, siginfo_t *info, void *context) {
  int result[2] = {signal, info->si_code};
  (void)context;
  _exit(write(result_fd, result, sizeof(result)) != sizeof(result));
}
static int observe(void (*fn)(void), int result[2]) {
  int descriptors[2];
  if (pipe(descriptors)) return 1;
  pid_t pid = fork();
  if (pid < 0) return 2;
  if (!pid) {
    close(descriptors[0]);
    result_fd = descriptors[1];
    struct sigaction action = {0};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGILL, &action, 0) || sigaction(SIGSEGV, &action, 0) ||
        sigaction(SIGBUS, &action, 0)) _exit(3);
    fn();
    _exit(4); // All fixtures deliberately have invalid addresses.
  }
  close(descriptors[1]);
  ssize_t bytes = read(descriptors[0], result, 2 * sizeof(int));
  close(descriptors[0]);
  int status;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) || bytes != 2 * sizeof(int)) return 5;
  return 0;
}
int main(void) {
  int expected[2], actual[2];
  if (observe(neverd_original, expected) ||
      observe(virtualization_test, actual)) return 1;
  return expected[0] != actual[0] || expected[1] != actual[1];
}
__asm__(".text\n.globl neverd_original\nneverd_original:\n"
)C";
  for (const auto Byte : Image.Segments.front().Data)
    OS << "\".byte " << unsigned(Byte) << "\\n\"\n";
  OS << ");\n";
  return Source;
}

// Compile the actual LLVM assembly and both emitted C routes, including all
// output constraints. Success fixtures execute against an independent expected
// value; selected system-memory fixtures instead compare faults with raw bytes.
void compileRoutes(BinaryImage &Image, bool NoOpt, va_t Address,
                   llvm::StringRef Mnemonic,
                   std::optional<uint64_t> Expected = std::nullopt,
                   bool OriginalBytes = false, bool CompareFaults = false,
                   llvm::StringRef CustomDriver = {}) {
  const bool Execute = Expected || CompareFaults || !CustomDriver.empty();
#if !defined(__linux__) || !defined(__x86_64__)
  if (Execute)
    GTEST_SKIP() << "Native execution fixture requires Linux x86-64";
#endif
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  auto LinkArguments = [&](llvm::StringRef Driver, llvm::StringRef Object,
                           llvm::StringRef Executable) {
    std::vector<llvm::StringRef> Args = {Compiler, Driver, Object, "-o",
                                         Executable};
    if (Image.Arch == Arch::X86)
      Args.insert(Args.end(), {"--target=i386-unknown-linux-gnu", "-nostdlib",
                               "-no-pie", "-fno-builtin", "-Wl,-e,_start"});
    return Args;
  };
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-x86-source", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  std::string High, LLVM, Native;
  std::vector<uint8_t> NativeObject;
  sourceRoutes(Image, NoOpt, Address, Mnemonic, &High, &LLVM, {}, {}, &Native,
               &NativeObject);
  if (::testing::Test::HasFailure())
    return;
  unsigned Route = 0;
  for (auto Source : {High, LLVM, Native}) {
    SCOPED_TRACE(Route);
    const bool IR = Route++ == 2;
    if (!IR)
      Source = freestandingC(std::move(Source));
    llvm::SmallString<128> Input(Directory), Output(Directory),
        ErrorFile(Directory), Driver(Directory), Executable(Directory);
    llvm::sys::path::append(Input, IR ? "source.ll" : "source.c");
    llvm::sys::path::append(Output, "source.o");
    llvm::sys::path::append(ErrorFile, "clang.err");
    llvm::sys::path::append(Driver, "driver.c");
    llvm::sys::path::append(Executable, "check");
    std::error_code Error;
    llvm::raw_fd_ostream OS(Input, Error);
    ASSERT_FALSE(Error);
    OS << Source;
    OS.close();
    ASSERT_FALSE(OS.has_error());
    if (Execute) {
      llvm::raw_fd_ostream DriverOS(Driver, Error);
      ASSERT_FALSE(Error);
      if (!CustomDriver.empty())
        DriverOS << CustomDriver;
      else if (CompareFaults) {
        ASSERT_EQ(Image.Arch, Arch::X64);
        DriverOS << faultDriver(Image);
      } else if (Image.Arch == Arch::X86) {
        DriverOS << R"C(
void *memcpy(void *d, const void *s, unsigned n) {
  for (unsigned i = 0; i < n; ++i) ((unsigned char *)d)[i] = ((const unsigned char *)s)[i];
  return d;
}
void *memset(void *d, int c, unsigned n) {
  for (unsigned i = 0; i < n; ++i) ((unsigned char *)d)[i] = c;
  return d;
}
extern unsigned virtualization_test(void);
void _start(void) {
  unsigned status = virtualization_test() != )C"
                 << *Expected << R"C(U;
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
)C";
      } else
        DriverOS << "extern unsigned long long virtualization_test(void);\n"
                    "int main(void) { return virtualization_test() != "
                 << *Expected << "ULL; }\n";
      DriverOS.close();
      ASSERT_FALSE(DriverOS.has_error());
    }
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      std::vector<llvm::StringRef> Arguments = {
          Compiler,
          Image.Arch == Arch::X86 ? "--target=i386-unknown-linux-gnu"
                                  : "--target=x86_64-unknown-linux-gnu",
          Optimization,
          "-c",
          Input,
          "-o",
          Output};
      // Match Codegen's x86 SSE2 baseline. Clang's plain i386 target has no
      // XMM register class for the emitted floating-point assembly operands.
      if (Image.Arch == Arch::X86)
        Arguments.push_back("-msse2");
      if (!IR)
        Arguments.insert(Arguments.end(),
                         {"-std=gnu11", "-ffreestanding",
                          "-fno-strict-aliasing",
                          "-Werror=implicit-function-declaration"});
      const std::optional<llvm::StringRef> Redirects[] = {
          std::nullopt, std::nullopt, ErrorFile.str()};
      const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                   std::nullopt, Redirects);
      const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
      ASSERT_EQ(Status, 0) << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                           : "")
                           << Source;
      if (Execute) {
        const int Linked = llvm::sys::ExecuteAndWait(
            Compiler, LinkArguments(Driver, Output, Executable), std::nullopt,
            Redirects);
        const auto LinkDiagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
        ASSERT_EQ(Linked, 0)
            << (LinkDiagnostics ? (*LinkDiagnostics)->getBuffer().str() : "");
        EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0)
            << Source;
      }
    }
  }
  if (Execute) {
    // The repository's LLVM fork is the native oracle. The system Clang above
    // independently checks emitted IR compatibility and compiles generated C.
    llvm::SmallString<128> Object(Directory), Driver(Directory),
        Executable(Directory);
    llvm::sys::path::append(Object, "neverd.o");
    llvm::sys::path::append(Driver, "driver.c");
    llvm::sys::path::append(Executable, "neverd-check");
    std::error_code Error;
    llvm::raw_fd_ostream OS(Object, Error);
    ASSERT_FALSE(Error);
    OS.write(reinterpret_cast<const char *>(NativeObject.data()),
             NativeObject.size());
    OS.close();
    ASSERT_FALSE(OS.has_error());
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  Compiler, LinkArguments(Driver, Object, Executable)),
              0);
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0);
    if (OriginalBytes) {
      llvm::SmallString<128> Assembly(Directory);
      llvm::sys::path::append(Assembly, "original.s");
      llvm::raw_fd_ostream Original(Assembly, Error);
      ASSERT_FALSE(Error);
      Original << ".text\n.globl virtualization_test\nvirtualization_test:\n";
      for (const auto Byte : Image.Segments.front().Data)
        Original << ".byte " << unsigned(Byte) << '\n';
      Original << ".section .note.GNU-stack,\"\",@progbits\n";
      Original.close();
      ASSERT_FALSE(Original.has_error());
      ASSERT_EQ(llvm::sys::ExecuteAndWait(
                    Compiler, LinkArguments(Driver, Assembly, Executable)),
                0);
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0);
    }
  }
}

std::vector<uint8_t> fpEncodingFunction(Arch A) {
  if (A == Arch::X86)
    return {0xb8, 3, 0, 0, 0, 0xf2, 0x0f, 0x2a, 0xc8, 0xb8, 7, 0, 0, 0, 0xf2,
            0x0f, 0x2a, 0xd0, 0xc4, 0xc1, 0x33, 0x58, 0xc2, // VADDSD;
                                                            // ignored
                                                            // B/vvvv[3].
            0xc4, 0xc1, 0x7b, 0x2c, 0xc0, // VCVTTSD2SI; ignored B.
            0xc3};
  return {0x48, 0x83, 0xec, 0x28, // SUB rsp,40.
          0x48, 0xb8, 0,    0,    0,    0,    0,    0,
          0xf0, 0x3f, 0x48, 0x89, 0x04, 0x24, // [rsp] = 1.0.
          0x48, 0xb8, 0,    0,    0,    0,    0,    0,
          0,    0x40, 0x48, 0x89, 0x44, 0x24, 0x08, // [rsp+8] = 2.0.
          0x66, 0x0f, 0x10, 0x04, 0x24,             // MOVUPD xmm0,[rsp].
          0xd5, 0x48, 0x8d, 0x04, 0x24,             // LEA r16,[rsp].
          0x66, 0xd5, 0xd0, 0x58, 0x00,             // ADDPD xmm0,[r16].
          0xf2, 0xd5, 0xc8, 0x2c, 0xc8,             // CVTTSD2SI r17,xmm0.
          0xd5, 0x48, 0x89, 0xc8,                   // MOV rax,r17.
          0x48, 0x83, 0xc4, 0x28, 0xc3};
}
} // namespace

TEST(X86FPEncodingSource, BothModesSurviveEveryFormatAndSourceRoute) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(NoOpt);
        auto Image = image(A, Format, fpEncodingFunction(A));
        ASSERT_NO_FATAL_FAILURE(sourceRoutes(Image, NoOpt, Entry, {}));
      }
}

TEST(X86FPEncodingSource, Rex2ExecutesThroughEveryRouteWithoutAPXHardware) {
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image =
        image(Arch::X64, BinaryFormat::ELF, fpEncodingFunction(Arch::X64));
    ASSERT_NO_FATAL_FAILURE(compileRoutes(Image, NoOpt, Entry, {}, 2));
  }
}

TEST(X86FPEncodingSource, Mode32IgnoredFieldsAgreeWithOriginalNativeBytes) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "Original VEX instruction oracle requires AVX";
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image =
        image(Arch::X86, BinaryFormat::ELF, fpEncodingFunction(Arch::X86));
    ASSERT_NO_FATAL_FAILURE(compileRoutes(Image, NoOpt, Entry, {}, 10, true));
  }
}

TEST(X86VectorEncodingSource, Mode32IgnoredExtensionsExecuteThroughEveryRoute) {
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Image =
        image(Arch::X86, BinaryFormat::ELF,
              {0xb8, 7,    0,    0,    0,    0x66, 0x0f, 0x6e, 0xc8, 0xb8,
               11,   0,    0,    0,    0x66, 0x0f, 0x6e, 0xd0, 0x62, 0xc1,
               0x35, 0x08, 0xfe, 0xc2, 0x66, 0x0f, 0x7e, 0xc0, 0xc3});
    // vpaddd xmm0, xmm1, xmm2: R', B and vvvv[3] are ignored in this mode.
    // No host AVX-512 feature is needed for the lifted integer arithmetic.
    compileRoutes(Image, NoOpt, Entry + 18, {}, 18);
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86VirtualizationSource,
     GeneratedAssemblyCompilesForBothWordSizesAtO0AndO2) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : Encodings) {
      const auto C = x86VirtualizationContract(E.Id);
      SCOPED_TRACE(C.Mnemonic);
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
      compileRoutes(Image, false, Entry + Offset, C.Mnemonic);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86SystemStateSource, EveryOwnedInstructionSurvivesBothSourceRoutes) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &E : systemEncodings(A)) {
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Image = image(A, Format, function(A, E.Bytes, Offset));
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic);
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86SystemStateSource, GeneratedAssemblyCompilesForBothWordSizesAtO0AndO2) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : systemEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
      compileRoutes(Image, false, Entry + Offset, E.Mnemonic);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86SystemMemorySource, BothSourceRoutesRetainEffectsAcrossAllFormats) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (const auto &E : systemMemoryEncodings(A)) {
          SCOPED_TRACE(E.Mnemonic);
          SCOPED_TRACE(static_cast<unsigned>(A));
          SCOPED_TRACE(static_cast<unsigned>(Format));
          SCOPED_TRACE(NoOpt);
          size_t Offset;
          auto Image = image(A, Format, function(A, E.Bytes, Offset));
          std::string High, LLVM;
          sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, &High, &LLVM);
          const bool Segmented =
              E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65;
          for (const auto *Source : {&High, &LLVM}) {
            if (Format == BinaryFormat::COFF && Source == &High && !Segmented &&
                (E.Id == Intrinsic::CetWrss || E.Id == Intrinsic::CetWruss)) {
              EXPECT_NE(Source->find("_" + std::string(E.Mnemonic) + "("),
                        std::string::npos)
                  << *Source;
              EXPECT_NE(Source->find("#include <intrin.h>"), std::string::npos);
              continue;
            }
            EXPECT_NE(Source->find("__asm__ volatile"), std::string::npos);
            EXPECT_NE(Source->find("\"memory\""), std::string::npos);
            if (Segmented)
              EXPECT_NE(
                  Source->find(E.Bytes.front() == 0x64 ? "%%fs:" : "%%gs:"),
                  std::string::npos)
                  << *Source;
          }
          if (::testing::Test::HasFailure())
            return;
        }
}

TEST(X86SystemMemorySource, GeneratedInstructionsCompileAtO0AndO2) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : systemMemoryEncodings(A)) {
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
      compileRoutes(Image, false, Entry + Offset, E.Mnemonic);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86SystemMemorySource, WindowsShadowWritesCompileWithMsvcIntrinsics) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-cet-coff", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  llvm::SmallString<128> Input(Directory), Object(Directory),
      ErrorFile(Directory);
  llvm::sys::path::append(Input, "shadow.c");
  llvm::sys::path::append(Object, "shadow.obj");
  llvm::sys::path::append(ErrorFile, "clang.err");
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : systemMemoryEncodings(A)) {
      if ((E.Id != Intrinsic::CetWrss && E.Id != Intrinsic::CetWruss) ||
          E.Bytes.front() == 0x64 || E.Bytes.front() == 0x65)
        continue;
      SCOPED_TRACE(E.Mnemonic);
      SCOPED_TRACE(static_cast<unsigned>(A));
      size_t Offset;
      auto Image = image(A, BinaryFormat::COFF, function(A, E.Bytes, Offset));
      std::string High;
      sourceRoutes(Image, false, Entry + Offset, E.Mnemonic, &High);
      std::error_code Error;
      llvm::raw_fd_ostream OS(Input, Error);
      ASSERT_FALSE(Error);
      OS << freestandingC(High);
      OS.close();
      ASSERT_FALSE(OS.has_error());
      for (const char *Optimization : {"-O0", "-O2"}) {
        const std::vector<llvm::StringRef> Arguments = {
            Compiler,
            A == Arch::X64 ? "--target=x86_64-pc-windows-msvc"
                           : "--target=i686-pc-windows-msvc",
            "-ffreestanding",
            "-fno-strict-aliasing",
            "-mshstk",
            "-Werror=implicit-function-declaration",
            Optimization,
            "-c",
            Input,
            "-o",
            Object};
        const std::optional<llvm::StringRef> Redirects[] = {
            std::nullopt, std::nullopt, ErrorFile.str()};
        const int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments,
                                                     std::nullopt, Redirects);
        const auto Diagnostics = llvm::MemoryBuffer::getFile(ErrorFile);
        ASSERT_EQ(Status, 0)
            << (Diagnostics ? (*Diagnostics)->getBuffer().str() : "") << High;
      }
    }
}

TEST(X86SystemMemorySource, FaultsMatchOriginalInstructionsAcrossEveryRoute) {
  // Feature/privilege/enablement checks must precede ordinary operand faults.
  // 0x8000000000000000 is noncanonical for both 48- and 57-bit addressing.
  for (const auto &E : std::vector<SystemEncoding>{
           {{0x0f, 0x38, 0xf6, 0x08}, Intrinsic::CetWrss, "wrssd"},
           {{0x48, 0x0f, 0x38, 0xf6, 0x08}, Intrinsic::CetWrss, "wrssq"},
           {{0x66, 0x0f, 0x38, 0xf5, 0x08}, Intrinsic::CetWruss, "wrussd"},
           {{0x66, 0x48, 0x0f, 0x38, 0xf5, 0x08},
            Intrinsic::CetWruss,
            "wrussq"},
           {{0xf2, 0x0f, 0x38, 0xf8, 0x08}, Intrinsic::Enqcmd, "enqcmd"},
           {{0xf3, 0x0f, 0x38, 0xf8, 0x08}, Intrinsic::Enqcmds, "enqcmds"},
           {{0x0f, 0x38, 0xf9, 0x08}, Intrinsic::X86DirectStore, "movdiri"},
           {{0x48, 0x0f, 0x38, 0xf9, 0x08},
            Intrinsic::X86DirectStore,
            "movdiri"},
           {{0x66, 0x0f, 0x38, 0xf8, 0x08},
            Intrinsic::X86DirectStore64B,
            "movdir64b"}}) {
    SCOPED_TRACE(E.Mnemonic);
    std::vector<uint8_t> Bytes = {0x48, 0xb8, 0,    0, 0, 0, 0, 0,
                                  0,    0x80, 0xb9, 1, 0, 0, 0};
    const auto Offset = Bytes.size();
    Bytes.insert(Bytes.end(), E.Bytes.begin(), E.Bytes.end());
    Bytes.push_back(0xc3);
    auto Image = image(Arch::X64, BinaryFormat::ELF, Bytes);
    for (bool NoOpt : {false, true}) {
      compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, std::nullopt,
                    false, true);
      if (::testing::Test::HasFailure())
        return;
    }
  }
}

TEST(X86SystemMemorySource, MalformedLLVMAssemblyCannotBypassTheContract) {
  for (const auto Id :
       {Intrinsic::CetWrss, Intrinsic::CetWruss, Intrinsic::Enqcmd,
        Intrinsic::Enqcmds, Intrinsic::X86DirectStore,
        Intrinsic::X86DirectStore64B})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Id));
      SCOPED_TRACE(Mutation);
      const bool Enqueue = Id == Intrinsic::Enqcmd || Id == Intrinsic::Enqcmds;
      const auto P =
          x86SystemStateAsm(Id, Arch::X64, 8, NdMemoryAddressSpace::Default);
      ASSERT_TRUE(P);
      llvm::LLVMContext Context;
      llvm::Module Module("system-memory-contract", Context);
      Module.setDataLayout("e-p:64:64");
      llvm::IRBuilder<> B(Context);
      auto *Pointer = llvm::PointerType::getUnqual(Context);
      std::vector<llvm::Type *> Types = {
          Mutation == 4 ? static_cast<llvm::Type *>(B.getInt64Ty()) : Pointer,
          isX86SystemStateAddressArgument(Id, 1)
              ? static_cast<llvm::Type *>(Pointer)
              : B.getInt64Ty()};
      llvm::Type *Return = Enqueue ? B.getInt8Ty() : B.getVoidTy();
      auto *Signature = llvm::FunctionType::get(Return, Types, false);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, "effect", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
      std::string Constraints = P->Constraints;
      if (Mutation == 1)
        Constraints.erase(Constraints.find(",~{memory}"));
      auto *Assembly = llvm::InlineAsm::get(
          Signature, P->Text + (Mutation == 3 ? "\n\tnop" : ""), Constraints,
          Mutation != 2, false,
          Mutation == 6 ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
      auto *Call =
          B.CreateCall(Assembly, {Function->getArg(0), Function->getArg(1)});
      if (Mutation == 5)
        Call->setOnlyReadsMemory();
      if (Mutation == 7)
        Call->setOnlyWritesMemory();
      if (Enqueue)
        B.CreateRet(Call);
      else
        B.CreateRetVoid();
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Arch::X64;
      Options.EmitIncludes = false;
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      if (Mutation == 0)
        EXPECT_TRUE(LLVMCEmitter().emit(Module, OS, Options)) << Text;
      else
        EXPECT_THROW(LLVMCEmitter().emit(Module, OS, Options),
                     std::invalid_argument);
    }
}

namespace {

std::vector<uint8_t> directStoreFunction(Arch A, bool Block, bool Wide,
                                         size_t &Offset) {
  std::vector<uint8_t> Bytes;
  if (A == Arch::X86)
    Bytes = {0x8b, 0x54, 0x24, 0x04, 0x8b, 0x4c, 0x24, 0x08};
  // Set CF/ZF before the direct store; both it and SFENCE preserve flags.
  Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xf9});
  Offset = Bytes.size();
  if (Block)
    Bytes.insert(Bytes.end(), {0x66, 0x0f, 0x38, 0xf8, 0x3e}); // rdi, [rsi]
  else {
    if (Wide)
      Bytes.push_back(0x48);
    Bytes.insert(
        Bytes.end(),
        {0x0f, 0x38, 0xf9, static_cast<uint8_t>(A == Arch::X86 ? 0x0a : 0x37)});
  }
  // Read completion only after an architecturally explicit store fence.
  Bytes.insert(Bytes.end(), {0x0f, 0xae, 0xf8, 0x0f, 0x92, 0xc0, 0x0f,
                             0x94, 0xc2, 0x0f, 0xb6, 0xc0, 0x0f, 0xb6,
                             0xd2, 0xc1, 0xe2, 0x08, 0x09, 0xd0, 0xc3});
  return Bytes;
}

std::string directStoreDriver(Arch A, bool Block, unsigned Width) {
  std::string Driver = "#define STORE_BYTES " + std::to_string(Width) + "\n";
  Driver += R"C(
static unsigned char bytes[256] __attribute__((aligned(64)));
static unsigned char expected[256];
)C";
  if (Block)
    Driver += R"C(
extern unsigned virtualization_test(void *, const void *);
static int check(void) {
  const unsigned sources[] = {0, 1, 8, 64, 65, 127};
  const unsigned destinations[] = {0, 64, 128};
  for (unsigned s = 0; s != sizeof(sources) / sizeof(*sources); ++s)
    for (unsigned d = 0; d != sizeof(destinations) / sizeof(*destinations); ++d) {
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        expected[i] = bytes[i] = (unsigned char)(i * 13 + s * 31 + d * 7);
      for (unsigned i = 0; i != 64; ++i)
        expected[destinations[d] + i] = bytes[sources[s] + i];
      if (virtualization_test(bytes + destinations[d], bytes + sources[s]) != 0x101)
        return 1;
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        if (bytes[i] != expected[i]) return 2;
    }
  return 0;
}
)C";
  else
    Driver += R"C(
extern unsigned virtualization_test(void *, __UINTPTR_TYPE__);
static int check(void) {
  const unsigned offsets[] = {0, 1, 3, 7, 15, 60, 63};
  const __UINTPTR_TYPE__ values[] = {0, (__UINTPTR_TYPE__)-1,
      (__UINTPTR_TYPE__)0xfedcba9876543210ULL, (__UINTPTR_TYPE__)0x8000000180000000ULL};
  for (unsigned o = 0; o != sizeof(offsets) / sizeof(*offsets); ++o)
    for (unsigned v = 0; v != sizeof(values) / sizeof(*values); ++v) {
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        expected[i] = bytes[i] = (unsigned char)(i * 13 + o * 7);
      for (unsigned i = 0; i != STORE_BYTES; ++i)
        expected[offsets[o] + i] = (unsigned char)(values[v] >> (8 * i));
      if (virtualization_test(bytes + offsets[o], values[v]) != 0x101) return 1;
      for (unsigned i = 0; i != sizeof(bytes); ++i)
        if (bytes[i] != expected[i]) return 2;
    }
  return 0;
}
)C";
  if (A == Arch::X86)
    Driver += R"C(
void *memcpy(void *dst, const void *src, unsigned size) {
  for (unsigned i = 0; i != size; ++i)
    ((unsigned char *)dst)[i] = ((const unsigned char *)src)[i];
  return dst;
}
void _start(void) {
  unsigned status = check();
  __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
  __builtin_unreachable();
}
)C";
  else
    Driver += "int main(void) { return check(); }\n";
  return Driver;
}

} // namespace

TEST(X86DirectStoreSource, ScalarStoresMatchOriginalBytesAndPreserveFlags) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("movdiri"))
    GTEST_SKIP() << "Original-byte execution requires host MOVDIRI";
  for (const auto A : {Arch::X86, Arch::X64})
    for (const bool Wide : {false, true}) {
      if (A == Arch::X86 && Wide)
        continue;
      size_t Offset;
      auto Image = image(A, BinaryFormat::ELF,
                         directStoreFunction(A, false, Wide, Offset));
      for (const bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(Wide);
        SCOPED_TRACE(NoOpt);
        compileRoutes(Image, NoOpt, Entry + Offset, "movdiri", std::nullopt,
                      true, false, directStoreDriver(A, false, Wide ? 8 : 4));
        if (::testing::Test::HasFailure())
          return;
      }
    }
}

TEST(X86DirectStoreSource, BlockStoresMatchOriginalBytesIncludingOverlap) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("movdir64b"))
    GTEST_SKIP() << "Original-byte execution requires host MOVDIR64B";
  size_t Offset;
  auto Image = image(Arch::X64, BinaryFormat::ELF,
                     directStoreFunction(Arch::X64, true, false, Offset));
  for (const bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    compileRoutes(Image, NoOpt, Entry + Offset, "movdir64b", std::nullopt, true,
                  false, directStoreDriver(Arch::X64, true, 64));
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86FenceSource, HardwareBarriersSurviveNativeLLVMOptimization) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const auto &E : std::vector<SystemEncoding>{
             {{0x0f, 0xae, 0xe8}, Intrinsic::Lfence, "lfence"},
             {{0x0f, 0xae, 0xf0}, Intrinsic::Mfence, "mfence"},
             {{0x0f, 0xae, 0xf8}, Intrinsic::Sfence, "sfence"}})
      for (const bool NoOpt : {false, true}) {
        size_t Offset;
        auto Image = image(A, BinaryFormat::ELF, function(A, E.Bytes, Offset));
        std::string Native;
        std::vector<uint8_t> Object;
        sourceRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic, nullptr, nullptr,
                     {}, {}, &Native, &Object);
        EXPECT_NE(Native.find("asm sideeffect \"" + std::string(E.Mnemonic) +
                              "\", \"~{memory}\""),
                  std::string::npos)
            << Native;
        EXPECT_NE(std::search(Object.begin(), Object.end(), E.Bytes.begin(),
                              E.Bytes.end()),
                  Object.end());
        compileRoutes(Image, NoOpt, Entry + Offset, E.Mnemonic);
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86SHA512Source, BothCAndLLVMExecuteWithoutSHA512Hardware) {
  const Words D = {0x0123456789abcdef, 0x8899aabbccddeeff, 0x1020304050607080,
                   0xfedcba9876543210};
  const Words S = {0xa1b2c3d4e5f60718, 0xf1e2d3c4b5a69788, 0x1122334455667788,
                   0x9988776655443322};
  const Words W = {0xffffffffffffffff, 0x55555555aaaaaaaa};
  for (const unsigned Opcode : {0xcc, 0xcd, 0xcb})
    for (const bool NoOpt : {false, true}) {
      SCOPED_TRACE(Opcode);
      SCOPED_TRACE(NoOpt);
      std::vector<uint8_t> Bytes = {0x48, 0x83, 0xec, 0x50};
      unsigned Offset = 0;
      for (const auto V :
           {D[0], D[1], D[2], D[3], S[0], S[1], S[2], S[3], W[0], W[1]}) {
        Bytes.insert(Bytes.end(), {0x48, 0xb8}); // movabs rax, value
        for (unsigned I = 0; I != 8; ++I)
          Bytes.push_back(V >> (8 * I));
        Bytes.insert(Bytes.end(),
                     {0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(Offset)});
        Offset += 8;
      }
      Bytes.insert(Bytes.end(),
                   {0xc5, 0xfe, 0x6f, 0x14, 0x24,         // ymm2 <- D
                    0xc5, 0xfe, 0x6f, 0x5c, 0x24, 0x20,   // ymm3 <- S
                    0xc5, 0xfa, 0x6f, 0x64, 0x24, 0x40}); // xmm4 <- W
      Bytes.insert(Bytes.end(),
                   {0xc4, 0xe2,
                    static_cast<uint8_t>(Opcode == 0xcb ? 0x67 : 0x7f),
                    static_cast<uint8_t>(Opcode),
                    static_cast<uint8_t>(Opcode == 0xcb ? 0xd4 : 0xd3), 0xc5,
                    0xfe, 0x7f, 0x14, 0x24,   // store ymm2
                    0x48, 0x8b, 0x04, 0x24}); // rax <- lane 0
      for (const uint8_t Disp : {8, 16, 24})
        Bytes.insert(Bytes.end(), {0x48, 0x33, 0x44, 0x24, Disp});
      Bytes.insert(Bytes.end(), {0x48, 0x83, 0xc4, 0x50, 0xc3});
      const auto R = sha512Reference(Opcode, D, S, W);
      auto Image = image(Arch::X64, BinaryFormat::ELF, std::move(Bytes));
      compileRoutes(Image, NoOpt, 0, {}, R[0] ^ R[1] ^ R[2] ^ R[3]);
      if (::testing::Test::HasFailure())
        return;
    }
}

TEST(X86SystemStateSource, FailedLarPreservesUpperDestinationInExecutableCode) {
  for (const bool NoOpt : {false, true}) {
    // A null selector fails LAR at CPL3 without touching privileged state.
    // The 32-bit destination must preserve *all* of RAX on failure.
    auto Image = image(Arch::X64, BinaryFormat::ELF,
                       {0x48, 0xb8, 0x89, 0x67, 0x45, 0x23, 0x01, 0xef, 0xcd,
                        0xab, 0x31, 0xc9, 0x0f, 0x02, 0xc1, 0xc3});
    compileRoutes(Image, NoOpt, Entry + 12, "lar",
                  UINT64_C(0xabcdef0123456789));
    if (::testing::Test::HasFailure())
      return;
  }
}

TEST(X86MaskedStoreSource, AllFourStoreFamiliesCompileThroughEveryRoute) {
  for (const bool Integer : {false, true})
    for (const bool Qword : {false, true})
      for (const bool Wide : {false, true}) {
        SCOPED_TRACE(Integer);
        SCOPED_TRACE(Qword);
        SCOPED_TRACE(Wide);
        std::vector<uint8_t> Bytes = {0x48, 0x83, 0xec, 0x60};
        for (unsigned I = 0; I != 12; ++I) {
          const uint64_t Value = I < 4 ? UINT64_C(0xa5a5a5a5a5a5a5a5)
                                 : I == 4
                                     ? (Qword ? UINT64_C(0x8000000000000000)
                                              : UINT64_C(0x80000000))
                                 : I < 8 ? 0
                                         : UINT64_C(0x5555555555555555);
          Bytes.insert(Bytes.end(), {0x48, 0xb8});
          for (unsigned J = 0; J != 8; ++J)
            Bytes.push_back(Value >> (8 * J));
          Bytes.insert(Bytes.end(),
                       {0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(8 * I)});
        }
        for (const std::vector<uint8_t> &Load :
             {std::vector<uint8_t>{0xc5,
                                   static_cast<uint8_t>(Wide ? 0xfe : 0xfa),
                                   0x6f, 0x14, 0x24},
              std::vector<uint8_t>{0xc5,
                                   static_cast<uint8_t>(Wide ? 0xfe : 0xfa),
                                   0x6f, 0x6c, 0x24, 0x20}})
          Bytes.insert(Bytes.end(), Load.begin(), Load.end());
        Bytes.insert(Bytes.end(),
                     {0xc4, 0xe2,
                      static_cast<uint8_t>(0x51 | (Wide ? 4 : 0) |
                                           (Integer && Qword ? 0x80 : 0)),
                      static_cast<uint8_t>(Integer ? 0x8e
                                           : Qword ? 0x2f
                                                   : 0x2e),
                      0x54, 0x24, 0x40, 0x48, 0x8b, 0x44, 0x24, 0x40, 0x48,
                      0x83, 0xc4, 0x60, 0xc3});
        auto Image = image(Arch::X64, BinaryFormat::ELF, std::move(Bytes));
        compileRoutes(Image, false, 0, {});
        if (::testing::Test::HasFailure())
          return;
      }
}

TEST(X86MaskedStoreSource,
     DynamicInactiveLanesNeitherAccessMemoryNorReadPoison) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "Native execution fixture requires Linux x86-64";
#endif
  const std::string Compiler = NEVERD_TEST_CLANG;
  if (Compiler.empty() || !llvm::sys::fs::exists(Compiler))
    GTEST_SKIP() << "Clang is unavailable";
  llvm::LLVMContext Context;
  llvm::Module Module("masked-memory", Context);
  Module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
  llvm::IRBuilder<> B(Context);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getInt1Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "masked_memory_test", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
  auto *Data = llvm::ConstantVector::get(
      {B.getInt64(0x123456789abcdef0), llvm::PoisonValue::get(B.getInt64Ty())});
  auto *Mask =
      B.CreateInsertElement(llvm::Constant::getNullValue(
                                llvm::FixedVectorType::get(B.getInt1Ty(), 2)),
                            F->getArg(1), B.getInt32(0));
  B.CreateMaskedStore(Data, F->getArg(0), llvm::Align(1), Mask);
  B.CreateRetVoid();
  auto *Load = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt64Ty(), {B.getPtrTy(), B.getInt1Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "masked_memory_load", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Load));
  auto *LoadMask =
      B.CreateInsertElement(llvm::Constant::getNullValue(
                                llvm::FixedVectorType::get(B.getInt1Ty(), 2)),
                            Load->getArg(1), B.getInt32(0));
  auto *Passthrough = llvm::ConstantVector::get(
      {B.getInt64(0x77), llvm::PoisonValue::get(B.getInt64Ty())});
  auto *Value = B.CreateMaskedLoad(Passthrough->getType(), Load->getArg(0),
                                   llvm::Align(1), LoadMask, Passthrough);
  B.CreateRet(B.CreateExtractElement(Value, B.getInt32(0)));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Original;
  llvm::raw_string_ostream OriginalOS(Original);
  Module.print(OriginalOS, nullptr);
  CEmitterOptions O;
  O.TheArch = Arch::X64;
  O.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, O));
  // Scalarization is private to the C projection.
  std::string After;
  llvm::raw_string_ostream AfterOS(After);
  Module.print(AfterOS, nullptr);
  EXPECT_EQ(Original, After);
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-masked-memory", Directory));
  auto Cleanup = llvm::scope_exit(
      [&] { (void)llvm::sys::fs::remove_directories(Directory); });
  llvm::SmallString<128> Input(Directory), Driver(Directory),
      Executable(Directory), Errors(Directory);
  llvm::sys::path::append(Input, "source.c");
  llvm::sys::path::append(Driver, "driver.c");
  llvm::sys::path::append(Executable, "check");
  llvm::sys::path::append(Errors, "clang.err");
  std::error_code Error;
  llvm::raw_fd_ostream SourceOS(Input, Error);
  ASSERT_FALSE(Error);
  SourceOS << Source;
  SourceOS.close();
  llvm::raw_fd_ostream DriverOS(Driver, Error);
  ASSERT_FALSE(Error);
  DriverOS << R"C(
#include <stdint.h>
extern void masked_memory_test(void *, _Bool);
extern uint64_t masked_memory_load(void *, _Bool);
int main(void) {
  masked_memory_test((void *)(uintptr_t)1, 0);
  if (masked_memory_load((void *)(uintptr_t)1, 0) != UINT64_C(0x77)) return 1;
  uint64_t data[2] = {0, UINT64_C(0xabcdef1234567890)};
  masked_memory_test(data, 1);
  return data[0] != UINT64_C(0x123456789abcdef0) ||
         data[1] != UINT64_C(0xabcdef1234567890) ||
         masked_memory_load(data, 1) != data[0];
}
)C";
  DriverOS.close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    const int Status = llvm::sys::ExecuteAndWait(
        Compiler,
        {Compiler, "-std=gnu11", Optimization, Input, Driver, "-o", Executable},
        std::nullopt, Redirects);
    auto Diagnostics = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Status, 0) << (Diagnostics ? (*Diagnostics)->getBuffer().str()
                                         : "")
                         << Source;
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}), 0) << Source;
  }
}
