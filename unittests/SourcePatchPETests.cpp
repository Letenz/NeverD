//===- SourcePatchPETests.cpp - Native PE source unwind contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
std::string takePE(const char *Value) {
  std::string Result = Value ? Value : "";
  neverd_free_string(Value);
  return Result;
}
int executePE(const std::vector<std::string> &Arguments) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, {}, 30);
}
uint64_t exportVA(const std::filesystem::path &Path, llvm::StringRef Wanted) {
  auto Parsed = llvm::object::ObjectFile::createObjectFile(Path.string());
  if (!Parsed) {
    ADD_FAILURE() << llvm::toString(Parsed.takeError());
    return 0;
  }
  auto *PE = llvm::cast<llvm::object::COFFObjectFile>(Parsed->getBinary());
  for (const auto &Export : PE->export_directories()) {
    llvm::StringRef Name;
    uint32_t RVA = 0;
    if (auto Error = Export.getSymbolName(Name)) {
      ADD_FAILURE() << llvm::toString(std::move(Error));
      return 0;
    }
    if (auto Error = Export.getExportRVA(RVA)) {
      ADD_FAILURE() << llvm::toString(std::move(Error));
      return 0;
    }
    if (Name == Wanted)
      return PE->getPE32PlusHeader()->ImageBase + RVA;
  }
  return 0;
}
using RuntimeRow = std::array<uint8_t, 12>;
std::set<RuntimeRow> runtimeRows(const std::filesystem::path &Path) {
  std::set<RuntimeRow> Result;
  auto Object = llvm::object::ObjectFile::createObjectFile(Path.string());
  if (!Object) {
    ADD_FAILURE() << llvm::toString(Object.takeError());
    return Result;
  }
  auto *PE = llvm::dyn_cast<llvm::object::COFFObjectFile>(Object->getBinary());
  if (!PE) {
    ADD_FAILURE() << "expected PE";
    return Result;
  }
  const auto *Directory = PE->getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!Directory || !Directory->Size || Directory->Size % 12) {
    ADD_FAILURE() << "missing or malformed runtime-function directory";
    return Result;
  }
  llvm::ArrayRef<uint8_t> Bytes;
  if (auto Error = PE->getRvaAndSizeAsBytes(Directory->RelativeVirtualAddress,
                                            Directory->Size, Bytes)) {
    ADD_FAILURE() << llvm::toString(std::move(Error));
    return Result;
  }
  while (!Bytes.empty()) {
    RuntimeRow Row{};
    std::copy_n(Bytes.begin(), Row.size(), Row.begin());
    EXPECT_TRUE(Result.insert(Row).second);
    Bytes = Bytes.drop_front(Row.size());
  }
  return Result;
}

class SourcePatchPE : public testing::Test {
protected:
  std::filesystem::path Directory, Original, Output;
  std::string Linker;
  neverd_session_t Session = nullptr;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  const char *Arguments[1] = {"-O0"};
  void SetUp() override {
    auto LLD = llvm::sys::findProgramByName("lld-link");
    if (!LLD) {
      auto ELFLinker = llvm::sys::findProgramByName("ld.lld");
      if (ELFLinker) {
        auto Candidate =
            std::filesystem::canonical(*ELFLinker).parent_path() / "lld-link";
        if (std::filesystem::exists(Candidate))
          LLD = Candidate.string();
      }
    }
    if (!LLD || std::string(NEVERD_TEST_CLANG).empty())
      GTEST_SKIP() << "cross Clang and lld-link are required";
    Linker = *LLD;
    llvm::SmallString<256> Temp;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-source-pe", Temp));
    Directory = Temp.str().str();
    Original = Directory / "original.exe";
    Output = Directory / "patched.exe";
    auto Source = Directory / "original.c", Object = Directory / "original.obj";
    std::ofstream(Source) << R"(
__declspec(dllexport) __declspec(noinline) int original_helper(int x) {
  volatile int y=x; return y+1;
}
__declspec(dllexport) __declspec(noinline) int target(int x) {
  volatile int y=x; return original_helper(y);
}
int entry(void) { return target(2); }
)";
    ASSERT_EQ(executePE({NEVERD_TEST_CLANG, "--target=x86_64-pc-windows-msvc",
                         "-O0", "-c", Source.string(), "-o", Object.string()}),
              0);
    ASSERT_EQ(
        executePE({*LLD, Object.string(), "/out:" + Original.string(),
                   "/entry:entry", "/subsystem:console", "/nodefaultlib"}),
        0);
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    Mapping.original_va = exportVA(Original, "target");
    ASSERT_NE(Mapping.original_va, 0u);
    Mapping.symbol = "replacement";
    Options.struct_size = sizeof(Options);
    Options.compiler_path = NEVERD_TEST_CLANG;
    Options.compiler_args = Arguments;
    Options.compiler_arg_count = 1;
    Options.mappings = &Mapping;
    Options.mapping_count = 1;
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }
  std::string error() { return takePE(neverd_last_error(Session)); }
  int patch(llvm::StringRef Source) {
    return neverd_patch_source(Session, Source.data(), Source.size(), &Options,
                               Output.string().c_str());
  }
};

TEST_F(SourcePatchPE, NonLeafReplacementKeepsOriginalAndAddsNewUnwindRows) {
  const auto Before = runtimeRows(Original);
  ASSERT_FALSE(Before.empty());
  ASSERT_EQ(patch("extern int original_helper(int); "
                  "int replacement(int x) { volatile int y=original_helper(x); "
                  "return y+39; }"),
            1)
      << error();
  const auto After = runtimeRows(Output);
  EXPECT_GT(After.size(), Before.size());
  for (const auto &Row : Before)
    EXPECT_TRUE(After.contains(Row));
  EXPECT_EQ(neverd_patch_trampoline_count(Session), 1);
#if defined(_WIN32) && defined(_M_X64)
  EXPECT_EQ(executePE({Original.string()}), 3);
  EXPECT_EQ(executePE({Output.string()}), 42);
#endif
}

TEST_F(SourcePatchPE, MissingLanguageRuntimeCannotPublish) {
  Options.language = NEVERD_SOURCE_PATCH_CXX;
  EXPECT_EQ(
      patch("extern \"C\" int original_helper(int); "
            "extern \"C\" int replacement(int x) { "
            "try { return original_helper(x); } catch (...) { return 42; } }"),
      0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchPE, SourceCannotClaimLiftedWindowsExceptionIdentity) {
  Options.input_kind = NEVERD_SOURCE_PATCH_LLVM_IR;
  EXPECT_EQ(patch(R"(
target triple = "x86_64-pc-windows-msvc"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
define i32 @replacement(i32 %x) !neverd.windows.eh !0 { ret i32 %x }
!0 = !{i32 0}
)"),
            0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchPE, WritablePointersHaveSeparateStorageAndBaseRelocations) {
  ASSERT_EQ(patch("extern int original_helper(int); "
                  "static volatile int value=40; "
                  "static volatile int *volatile pointer=&value; "
                  "int replacement(int x) { ++*pointer; return "
                  "*pointer+original_helper(x); }"),
            1)
      << error();
  auto Parsed = llvm::object::ObjectFile::createObjectFile(Output.string());
  ASSERT_TRUE(bool(Parsed));
  auto *PE = llvm::cast<llvm::object::COFFObjectFile>(Parsed->getBinary());
  const auto *Relocs = PE->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
  ASSERT_NE(Relocs, nullptr);
  EXPECT_GE(Relocs->Size, 12u);
  bool Data = false, Metadata = false;
  for (const auto &Section : PE->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(bool(Name));
    if (*Name != ".nddata" && *Name != ".ndmeta")
      continue;
    auto *Header = PE->getCOFFSection(Section);
    EXPECT_EQ(Header->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE, 0u);
    EXPECT_EQ(bool(Header->Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE),
              *Name == ".nddata");
    Data |= *Name == ".nddata";
    Metadata |= *Name == ".ndmeta";
  }
  EXPECT_TRUE(Data && Metadata);
#if defined(_WIN32) && defined(_M_X64)
  EXPECT_EQ(executePE({Original.string()}), 3);
  EXPECT_EQ(executePE({Output.string()}), 44);
#endif
}

TEST_F(SourcePatchPE, WrongEHModelIsRejectedBeforeWindowsCodegen) {
  Options.input_kind = NEVERD_SOURCE_PATCH_LLVM_IR;
  EXPECT_EQ(patch(R"(
target triple = "x86_64-pc-windows-msvc"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
declare i32 @__CxxFrameHandler3(...)
declare void @callee()
define i32 @replacement(i32 %x) personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @callee() to label %done unwind label %failed
done:
  ret i32 %x
failed:
  %exception = landingpad {ptr,i32} cleanup
  resume {ptr,i32} %exception
}
)"),
            0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchPE, ActiveCFGRequiresItsOwnSourceContract) {
  ASSERT_EQ(executePE({Linker, (Directory / "original.obj").string(),
                       "/out:" + Original.string(), "/entry:entry",
                       "/subsystem:console", "/nodefaultlib", "/guard:cf"}),
            0);
  ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
      << error();
  EXPECT_EQ(patch("int replacement(int x) { return x+40; }"), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchPE, FH3RuntimePreservesCatchesCleanupRethrowAndNoexcept) {
  const char *CRT = std::getenv("NEVERD_TEST_WINDOWS_CRT_LIB");
  const char *UCRT = std::getenv("NEVERD_TEST_WINDOWS_UCRT_LIB");
  const char *SDK = std::getenv("NEVERD_TEST_WINDOWS_SDK_LIB");
  const char *Runner = std::getenv("NEVERD_TEST_WINDOWS_RUNNER");
  if (!CRT || !UCRT || !SDK) {
    if (std::getenv("NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME"))
      FAIL() << "required Windows runtime inputs are not configured";
    GTEST_SKIP()
        << "Windows x64 CRT, UCRT and SDK library directories required";
  }
#if !defined(_WIN32) || !(defined(_M_X64) || defined(__x86_64__))
  if (!Runner || !*Runner)
    GTEST_SKIP() << "matching Windows runtime runner required";
#endif
  Original = Directory / "original-eh.exe";
  auto Source = Directory / "original-eh.cpp",
       Object = Directory / "original-eh.obj";
  std::ofstream(Source) << R"(
extern "C" __declspec(dllimport) void _exit(int);
using terminate_handler=void(*)();
extern "C" __declspec(dllimport) terminate_handler set_terminate(terminate_handler);
extern "C" __declspec(dllexport) int cleanup_count=0;
struct Guard { ~Guard() { ++cleanup_count; } };
extern "C" __declspec(dllexport) __declspec(noinline) int original_thrower(int x) {
  if(x) throw x; return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) int target(int x) {
  volatile int y=x; if(y==-999) throw y; return y+1;
}
__declspec(noinline) int reraiser(int x) {
  try {return original_thrower(x);} catch(...) {Guard g; throw;}
}
extern "C" __declspec(dllexport) __declspec(noinline) int original_noexcept(int x) noexcept {
  return original_thrower(x);
}
void on_terminate() { _exit(77); }
int main(int argc,char **argv) {
  set_terminate(on_terminate);
  int x=argc>1?argv[1][0]-'0':0;
  if(x==8) return original_noexcept(x);
  try {if(x==9) return reraiser(9); return target(x);}
  catch(int value) {return value+cleanup_count*10;}
})";
  ASSERT_EQ(executePE({NEVERD_TEST_CLANG, "--driver-mode=g++",
                       "--target=x86_64-pc-windows-msvc", "-O0",
                       "-fms-runtime-lib=dll", "-c", Source.string(), "-o",
                       Object.string()}),
            0);
  ASSERT_EQ(executePE({Linker, Object.string(), "/out:" + Original.string(),
                       "/subsystem:console", "/export:??_7type_info@@6B@,DATA",
                       std::string("/libpath:") + CRT,
                       std::string("/libpath:") + UCRT,
                       std::string("/libpath:") + SDK}),
            0);
  auto Run = [&](const std::filesystem::path &Path, unsigned Argument) {
    std::vector<std::string> Command;
    if (Runner && *Runner)
      Command.push_back(Runner);
    Command.push_back(Path.string());
    Command.push_back(std::to_string(Argument));
    return executePE(Command);
  };
  ASSERT_EQ(Run(Original, 0), 1);
  ASSERT_EQ(Run(Original, 8), 77);
  ASSERT_EQ(Run(Original, 9), 19);
  ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
      << error();
  Mapping.original_va = exportVA(Original, "target");
  ASSERT_NE(Mapping.original_va, 0u);
  neverd_source_patch_binding_v1 Bindings[2]{};
  Bindings[0].symbol = "cleanup_count";
  Bindings[0].original_va = exportVA(Original, "cleanup_count");
  Bindings[0].kind = NEVERD_SOURCE_BIND_WRITABLE_DATA;
  Bindings[0].size = 4;
  Bindings[1].symbol = "??_7type_info@@6B@";
  Bindings[1].original_va = exportVA(Original, Bindings[1].symbol);
  Bindings[1].kind = NEVERD_SOURCE_BIND_READ_ONLY_DATA;
  Bindings[1].size = 8;
  ASSERT_NE(Bindings[0].original_va, 0u);
  ASSERT_NE(Bindings[1].original_va, 0u);
  Options.bindings = Bindings;
  Options.binding_count = 2;
  Options.language = NEVERD_SOURCE_PATCH_CXX;
  ASSERT_EQ(patch(R"(
extern "C" int cleanup_count;
extern "C" int original_thrower(int);
struct Guard { ~Guard() { ++cleanup_count; } };
__declspec(noinline) int broken() noexcept { throw 31; }
extern "C" int replacement(int x) {
  if(x==5) return broken();
  if(x==0) return 40;
  if(x==1) {try {throw 7;} catch(int value) {return value+3;}}
  if(x==2) {Guard g; throw 11;}
  if(x==3) {Guard g; return original_thrower(17);}
  try {throw 21;} catch(...) {Guard g; throw;}
})"),
            1)
      << error();
  for (const auto [Mode, Expected] :
       {std::pair{0, 40}, {1, 10}, {2, 21}, {3, 27}, {4, 31}, {5, 77}, {9, 19}})
    EXPECT_EQ(Run(Output, Mode), Expected) << "mode " << Mode;
}
} // namespace
