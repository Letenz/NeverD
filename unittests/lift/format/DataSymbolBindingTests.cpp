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
#include <map>
#include <stdexcept>
#include <string>

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
                   bool StandardIO = false, bool Sectionless = false,
                   bool LoopCall = false) {
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
    if (LoopCall)
      Out << "extern int abs(int);\n"
             "__attribute__((no_builtin(\"abs\"))) int call_loop(void) {\n"
             "  int result=0;\n"
             "  for(int i=0;i<4;++i) result += abs(shared_counter+i);\n"
             "  return result;\n}\n";
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

  // Pointer variables a library starts at `pick`, which a second library
  // defines, so the dynamic symbol is an undefined function.  The calls are
  // not tail calls, and neither passes an argument: what they test is which
  // function they reach.
  fs::path hookLibrary(llvm::StringRef Target) {
    auto Build = [&](llvm::StringRef Name, llvm::StringRef Code,
                     std::vector<std::string> Inputs) {
      auto Source = tmpFile(Target.str() + "-" + Name.str() + ".c");
      auto Object = tmpFile(Target.str() + "-" + Name.str() + ".o");
      auto Library = tmpFile(Target.str() + "-" + Name.str() + ".so");
      std::ofstream(Source) << Code.str();
      auto Compile = exec(NEVERD_TEST_CLANG,
                          {"--target=" + Target.str(), "-fPIC", "-O1", "-c",
                           Source.string(), "-o", Object.string()});
      EXPECT_TRUE(Compile.ok()) << Compile.err;
      Inputs.insert(Inputs.begin(), Object.string());
      Inputs.insert(Inputs.begin(), "-shared");
      Inputs.push_back("-o");
      Inputs.push_back(Library.string());
      auto Link = exec("ld.lld", Inputs);
      EXPECT_TRUE(Link.ok()) << Link.err;
      return Library;
    };
    const auto Pick = Build("pick", "int pick(void) { return 5; }\n", {});
    return Build("hook",
                 "int pick(void);\n"
                 "static int (*hook)(void) = pick;\n"
                 "static int (*volatile spare)(void) = pick;\n"
                 "int call_hook(void) { return hook() + 1; }\n"
                 "int call_spare(void) { return spare() + 1; }\n"
                 "void set_hooks(int (*f)(void)) { hook = f; spare = f; }\n",
                 {Pick.string()});
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

TEST_F(DataSymbolBindingTest, PICLoopCallsKeepTheirStackArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(library("i386-linux-gnu", true, false, false, true));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto C = source(*Loaded, false, {"call_loop"});
  EXPECT_NE(C.find("shared_counter"), std::string::npos) << C;
  EXPECT_NE(C.find("abs("), std::string::npos) << C;
  EXPECT_EQ(C.find("unknown value"), std::string::npos) << C;
  EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
  // PIC loads can populate the shared definition index before the loop's
  // stack PHIs are visited. The abs prototype verifies that its pushed
  // argument survives either lookup order.
  const auto Output = tmpFile("pic-loop.c");
  std::ofstream(Output) << "extern int abs(int);\n" << C;
  const auto Compile =
      exec(NEVERD_TEST_CLANG, {"--target=i386-linux-gnu", "-ffreestanding",
                               "-Werror=implicit-function-declaration", "-c",
                               Output.string(), "-o", Output.string() + ".o"});
  EXPECT_TRUE(Compile.ok()) << Compile.err << C;
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

TEST_F(DataSymbolBindingTest, PointerVariablesStartAtExternalFunctions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(hookLibrary(Target));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    // The variables hold `pick` without being its import slots.
    const auto Pointers = collectExternalFunctionPointers(*Loaded);
    ASSERT_EQ(Pointers.size(), 2u);
    const auto Imports = Loaded->collectImportStorageSlots();
    for (const auto &[Slot, Name] : Pointers) {
      EXPECT_EQ(Name, "pick");
      EXPECT_EQ(Imports.Slots.count(Slot), 0u);
    }
    // Each call goes through what the variable holds then: `pick` at first,
    // and the function stored there afterwards.
    const auto C =
        source(*Loaded, true, {"call_hook", "call_spare", "set_hooks"});
    EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
    const auto Output = tmpFile(std::string(Target) + "-hooks.c");
    std::ofstream(Output) << C;
    const auto Program = tmpFile(std::string(Target) + "-hooks");
    std::vector<std::string> Args = {"--target=" + std::string(Target), "-O2",
                                     "-ffreestanding", "-fno-strict-aliasing",
                                     Output.string()};
    const bool RunNative = CanRunX64ELF && Loaded->Arch == Arch::X64;
    if (RunNative) {
      const auto Harness = tmpFile("hooks-main.c");
      std::ofstream(Harness)
          << "int call_hook(void); int call_spare(void);\n"
             "void set_hooks(int (*)(void));\n"
             "int pick(void) { return 5; }\n"
             "static int other(void) { return 41; }\n"
             "int main(void) {\n"
             "  if (call_hook() != 6 || call_spare() != 6)\n"
             "    return 1;\n"
             "  set_hooks(other);\n"
             "  return call_hook() != 42 || call_spare() != 42;\n"
             "}\n";
      Args.push_back(Harness.string());
    } else {
      Args.push_back("-c");
    }
    Args.push_back("-o");
    Args.push_back(Program.string());
    auto Compile = exec(NEVERD_TEST_CLANG, Args);
    EXPECT_TRUE(Compile.ok()) << Compile.err << C;
    if (RunNative && Compile.ok())
      EXPECT_TRUE(exec(Program.string(), {}).ok()) << C;
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

TEST(DataSymbolBindings, ExternalFunctionPointersNeedAnExactAbsoluteSymbol) {
  struct Target {
    Arch TheArch;
    Bitness Bits;
    uint32_t Absolute;
    uint32_t Binding;
    bool Rela;
  };
  for (const Target &T :
       {Target{Arch::X64, Bitness::Bits64, llvm::ELF::R_X86_64_64,
               llvm::ELF::R_X86_64_GLOB_DAT, true},
        Target{Arch::X86, Bitness::Bits32, llvm::ELF::R_386_32,
               llvm::ELF::R_386_GLOB_DAT, false},
        Target{Arch::ARM, Bitness::Bits32, llvm::ELF::R_ARM_ABS32,
               llvm::ELF::R_ARM_GLOB_DAT, false},
        Target{Arch::AArch64, Bitness::Bits64, llvm::ELF::R_AARCH64_ABS64,
               llvm::ELF::R_AARCH64_GLOB_DAT, true}}) {
    SCOPED_TRACE(static_cast<int>(T.TheArch));
    BinaryImage Image;
    Image.Format = BinaryFormat::ELF;
    Image.Arch = T.TheArch;
    Image.Bits = T.Bits;
    Segment Data;
    Data.VA = 0x4000;
    Data.Size = 16;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.assign(Data.Size, 0);
    Image.Segments.push_back(Data);
    RelocationEntry Relocation;
    Relocation.Address = Data.VA;
    Relocation.Type = T.Absolute;
    Relocation.HasExplicitAddend = T.Rela;
    Relocation.ELF.emplace();
    auto &Symbol = Relocation.ELF->Symbol.emplace();
    Symbol.Name = "exit";
    Symbol.Binding = llvm::ELF::STB_GLOBAL;
    Symbol.Type = llvm::ELF::STT_FUNC;
    Image.Relocations.push_back(Relocation);
    auto &Record = Image.Relocations[0];
    auto &Word = Image.Segments[0].Data;
    const std::map<va_t, std::string> Exit{{Data.VA, "exit"}};
    EXPECT_EQ(collectExternalFunctionPointers(Image), Exit);

    // An addend points past the entry: RELA states it, REL keeps it in the
    // word.
    if (T.Rela)
      Record.Addend = 8;
    else
      Word[0] = 8;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.Addend = 0;
    Word[0] = 0;
    if (T.Rela) {
      Word[0] = 0xa5; // RELA ignores the file's bytes.
      EXPECT_EQ(collectExternalFunctionPointers(Image), Exit);
      Word[0] = 0;
    }

    // A GOT binding is import storage, not a pointer variable.
    Record.Type = T.Binding;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.Type = T.Absolute;

    // Data, untyped symbols, weak functions the loader may leave null, and
    // functions the file defines are not external functions.
    for (unsigned Type : {llvm::ELF::STT_OBJECT, llvm::ELF::STT_NOTYPE}) {
      Record.ELF->Symbol->Type = Type;
      EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    }
    Record.ELF->Symbol->Type = llvm::ELF::STT_FUNC;
    Record.ELF->Symbol->Binding = llvm::ELF::STB_WEAK;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.ELF->Symbol->Binding = llvm::ELF::STB_GLOBAL;
    Record.ELF->Symbol->SectionIndex = 1;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.ELF->Symbol->SectionIndex = llvm::ELF::SHN_UNDEF;
    EXPECT_EQ(collectExternalFunctionPointers(Image), Exit);

    // A word two relocations start at different functions is no one's.
    RelocationEntry Abort = Record;
    Abort.ELF->Symbol->Name = "abort";
    Image.Relocations.push_back(std::move(Abort));
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Image.Relocations.pop_back();

    // A relocatable object's relocations are not the dynamic loader's.
    Image.IsRelocatable = true;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
  }
}
} // namespace
