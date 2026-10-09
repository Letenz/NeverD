//===- DataSymbolBindingTests.cpp - Named dynamic data pointers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "NeverDLiftFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/DataSymbolBinding.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <iterator>
#include <stdexcept>

using namespace neverd;
namespace {
#if defined(__linux__) && defined(__x86_64__)
constexpr bool CanRunX64ELF = true;
#else
constexpr bool CanRunX64ELF = false;
#endif

class DataSymbolBindingTest : public NeverDLiftTest {
protected:
  fs::path library(llvm::StringRef Target, bool Relro = true,
                   bool StandardIO = false, bool Sectionless = false) {
    auto Source = tmpFile(Target.str() + ".c");
    auto Object = tmpFile(Target.str() + ".o");
    auto Library = tmpFile(Target.str() + ".so");
    std::ofstream Out(Source);
    Out << "extern int shared_counter; int counter_def = 5;\n"
           "int bump(void) { return ++shared_counter; }\n"
           "int readdef(void) { return counter_def; }\n";
    if (StandardIO)
      Out << "#include <stdio.h>\n"
             "int put(const char *s) { return fputs(s, stdout); }\n";
    Out.close();
    auto Compile =
        exec(NEVERD_TEST_CLANG, {"--target=" + Target.str(), "-fPIC", "-O1",
                                 "-c", Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Compile.ok()) << Compile.err;
    auto Link = exec("ld.lld", {"-shared", "-z", Relro ? "relro" : "norelro",
                                Object.string(), "-o", Library.string()});
    EXPECT_TRUE(Link.ok()) << Link.err;
    if (Sectionless) {
      std::ifstream Input(Library, std::ios::binary);
      std::vector<char> Bytes(std::istreambuf_iterator<char>(Input), {});
      Input.close();
      auto Strip = [&]<typename Header>() {
        Header H;
        std::memcpy(&H, Bytes.data(), sizeof(H));
        H.e_shoff = 0;
        H.e_shnum = 0;
        H.e_shstrndx = 0;
        std::memcpy(Bytes.data(), &H, sizeof(H));
      };
      if (Bytes[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS64)
        Strip.template operator()<llvm::ELF::Elf64_Ehdr>();
      else
        Strip.template operator()<llvm::ELF::Elf32_Ehdr>();
      std::ofstream Output(Library, std::ios::binary | std::ios::trunc);
      Output.write(Bytes.data(), Bytes.size());
    }
    return Library;
  }

  std::string source(const BinaryImage &Image, bool LLVM,
                     std::initializer_list<const char *> Functions,
                     bool Only = false) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.SourceProjection = LLVM;
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    for (const auto *Name : Functions) {
      auto *Symbol = Image.findSymbol(Name);
      EXPECT_NE(Symbol, nullptr);
      if (Symbol)
        Options.OnlyFunctionEntries.insert(Symbol->Addr);
    }
    auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
    CEmitterOptions Emit;
    Emit.TheArch = Image.Arch;
    Emit.Format = Image.Format;
    Emit.Image = &Image;
    Emit.EmitIncludes = Image.Arch == Arch::X64;
    std::string C = Emit.EmitIncludes ? "" : "#include <stdint.h>\n";
    llvm::raw_string_ostream OS(C);
    if (LLVM) {
      EXPECT_NE(Result.LlvmModule, nullptr);
      if (Result.LlvmModule) {
        const llvm::Function *Function = nullptr;
        if (Only)
          for (const auto &F : *Result.LlvmModule)
            if (!F.isDeclaration()) {
              Function = &F;
              break;
            }
        EXPECT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, Emit, nullptr,
                                        &Image, Function));
      }
    } else {
      EXPECT_EQ(Result.HighFuncs.size(), Functions.size());
      EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Emit));
    }
    return C;
  }
};

TEST_F(DataSymbolBindingTest, PICObjectsKeepTheirIdentityAcrossArchitectures) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    for (bool Sectionless : {false, true}) {
      SCOPED_TRACE(Sectionless);
      auto Loaded = loadBinary(library(Target, true, false, Sectionless));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      const auto Bindings = collectDataSymbolBindings(*Loaded);
      ASSERT_EQ(Bindings.size(), 2u);
      for (const auto &[Slot, Binding] : Bindings) {
        EXPECT_TRUE(Binding.Immutable);
        EXPECT_EQ(Binding.Definition.has_value(),
                  Binding.Name == "counter_def");
      }
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        const auto C = source(*Loaded, LLVM, {"bump", "readdef"});
        EXPECT_NE(C.find("shared_counter"), std::string::npos) << C;
        // LLVM may fold a private rebuilt definition whose only use is a read.
        EXPECT_TRUE(C.find("counter_def") != std::string::npos ||
                    (LLVM && C.find("return 5;") != std::string::npos))
            << C;
        EXPECT_EQ(C.find("unknown value"), std::string::npos) << C;
        const auto Output =
            tmpFile(std::string(Target) + (LLVM ? "-llvm.c" : "-high.c"));
        std::ofstream(Output) << C;
        const auto Compile = exec(
            NEVERD_TEST_CLANG,
            {"--target=" + std::string(Target), "-ffreestanding",
             "-fno-strict-aliasing", "-Werror=implicit-function-declaration",
             "-c", Output.string(), "-o", Output.string() + ".o"});
        EXPECT_TRUE(Compile.ok()) << Compile.err << C;
      }
    }
  }
}

TEST_F(DataSymbolBindingTest, ExternalDataRunsAndDoesNotConflictWithHeaders) {
  if (!CanRunX64ELF)
    GTEST_SKIP() << "ELF stdio linkage execution requires a Linux x64 host";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(library("x86_64-linux-gnu", true, true));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto Harness = tmpFile("harness.c");
  std::ofstream(Harness)
      << "int shared_counter=40; int bump(void); int put(const char*);\n"
         "int main(void) { if(bump()!=41 || shared_counter!=41) return 1;"
         "return put(\"data-binding\\n\")<0; }\n";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto C = source(*Loaded, LLVM, {"bump", "put"});
    const auto Output = tmpFile(LLVM ? "llvm.c" : "high.c");
    std::ofstream(Output) << "#include <stdio.h>\n" << C;
    const auto Program = tmpFile(LLVM ? "llvm" : "high");
    auto Compile =
        exec(NEVERD_TEST_CLANG, {"-O2", "-fno-strict-aliasing", Output.string(),
                                 Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Compile.ok()) << Compile.err << C;
    const auto Run = exec(Program.string(), {});
    EXPECT_TRUE(Run.ok()) << Run.err;
    EXPECT_EQ(Run.out, "data-binding\n");
  }
  // Single-function LLVM C exports need the same declarations as a module.
  const auto Single = source(*Loaded, true, {"bump"}, true);
  EXPECT_NE(Single.find("extern unsigned char neverd_data_shared_counter[]"),
            std::string::npos)
      << Single;
}

TEST_F(DataSymbolBindingTest, WritableSlotsRetainStorageAndSymbolInitializer) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(library(Target, false));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (const auto &[Slot, Binding] : collectDataSymbolBindings(*Loaded))
      EXPECT_FALSE(Binding.Immutable);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      const auto C = source(*Loaded, LLVM, {"bump", "readdef"});
      if (!LLVM) {
        EXPECT_NE(C.find("got_shared_counter"), std::string::npos) << C;
        EXPECT_NE(C.find("= (uintptr_t)neverd_data_shared_counter"),
                  std::string::npos)
            << C;
      }
      const auto Output = tmpFile(LLVM ? "writable-llvm.c" : "writable-high.c");
      std::ofstream(Output) << C
                            << "\nint shared_counter=40;\n"
                               "int main(void){ return bump()!=41 || "
                               "shared_counter!=41 || readdef()!=5; }\n";
      const auto Program = tmpFile(LLVM ? "writable-llvm" : "writable-high");
      std::vector<std::string> Args = {"--target=" + std::string(Target),
                                       "-O2",
                                       "-ffreestanding",
                                       "-fno-strict-aliasing",
                                       Output.string(),
                                       "-o",
                                       Program.string()};
      const bool RunNative = CanRunX64ELF && Loaded->Arch == Arch::X64;
      if (!RunNative)
        Args.push_back("-c");
      else {
        Args.push_back("-fsanitize=undefined");
        Args.push_back("-fno-sanitize-recover=undefined");
      }
      auto Compile = exec(NEVERD_TEST_CLANG, Args);
      ASSERT_TRUE(Compile.ok()) << Compile.err << C;
      if (RunNative)
        EXPECT_TRUE(exec(Program.string(), {}).ok());
    }
  }
}

TEST_F(DataSymbolBindingTest, OpaqueAliasesKeepByteLoadAndStoreSemantics) {
  if (!CanRunX64ELF)
    GTEST_SKIP() << "ELF alias linkage execution requires a Linux x64 host";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang";
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
@stored_byte = external global i8, !neverd.data-symbol !0
define i8 @bump_byte() {
  %value = load i8, ptr @stored_byte
  %next = add i8 %value, 1
  store i8 %next, ptr @stored_byte
  ret i8 %next
}
!0 = !{}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Emit;
  Emit.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Emit));
  const auto File = tmpFile("byte.c");
  std::ofstream(File)
      << Source
      << "\nunsigned char stored_byte=255;\n"
         "int main(void){return bump_byte()!=0 || stored_byte!=0;}\n";
  const auto Program = tmpFile("byte");
  auto Compile =
      exec(NEVERD_TEST_CLANG,
           {"-O2", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
            File.string(), "-o", Program.string()});
  ASSERT_TRUE(Compile.ok()) << Compile.err << Source;
  EXPECT_TRUE(exec(Program.string(), {}).ok());
}

TEST(DataSymbolBindings, ExactSymbolKindAddendAndPermissionsOwnTheBinding) {
  BinaryImage Image;
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = 0x1000;
  Data.Size = 32;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(Data.Size, 0xa5); // File bytes are not a RELA addend.
  Image.Segments.push_back(Data);
  RelocationEntry Relocation;
  Relocation.Address = Data.VA;
  Relocation.Type = llvm::ELF::R_AARCH64_GLOB_DAT;
  Relocation.Addend = -8;
  Relocation.HasExplicitAddend = true;
  Relocation.SymbolName =
      "wrong_name"; // The exact r_sym record is authoritative.
  Relocation.ELF.emplace();
  auto &Symbol = Relocation.ELF->Symbol.emplace();
  Symbol.Name = "weak_data";
  Symbol.Binding = llvm::ELF::STB_WEAK;
  Symbol.Type = llvm::ELF::STT_OBJECT;
  Image.Relocations.push_back(Relocation);
  auto Binding = [&] { return collectDataSymbolBindings(Image).at(Data.VA); };
  EXPECT_EQ(Binding().Name, "weak_data");
  EXPECT_TRUE(Binding().Weak);
  EXPECT_FALSE(Binding().Definition);
  EXPECT_EQ(Binding().Addend, -8);
  EXPECT_FALSE(Binding().Immutable);
  Image.ELFMetadata.emplace();
  Image.ELFMetadata->ProgramHeaders.push_back(
      {llvm::ELF::PT_GNU_RELRO, 0, 0, Data.VA, 7, 7, 1});
  EXPECT_FALSE(Binding().Immutable); // A partial pointer is not read-only.
  Image.ELFMetadata->ProgramHeaders.back().MemorySize = 8;
  EXPECT_TRUE(Binding().Immutable);
  for (unsigned Kind :
       {llvm::ELF::STT_FUNC, llvm::ELF::STT_GNU_IFUNC, llvm::ELF::STT_TLS}) {
    Image.Relocations[0].ELF->Symbol->Type = Kind;
    EXPECT_TRUE(collectDataSymbolBindings(Image).empty());
  }
  Image.Relocations[0].ELF->Symbol->Type = llvm::ELF::STT_NOTYPE;
  EXPECT_EQ(Binding().Name, "weak_data");
  Image.Relocations[0].HasExplicitAddend = false;
  EXPECT_TRUE(collectDataSymbolBindings(Image).empty());
  Image.Arch = Arch::X64;
  Image.Relocations[0].Type = llvm::ELF::R_X86_64_GLOB_DAT;
  EXPECT_EQ(Binding().Addend, 0); // x86 GLOB_DAT ignores the in-place bytes.
  Image.Relocations.push_back(Image.Relocations[0]);
  Image.Relocations.back().ELF->Symbol->Name = "conflicting_data";
  EXPECT_THROW(collectDataSymbolBindings(Image), std::invalid_argument);
}
} // namespace
