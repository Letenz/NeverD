//===- SourcePatchObjectTests.cpp - Native object input contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace {
using ELF = llvm::object::ELF64LE;

std::string read(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input), {}};
}
std::string take(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}
int run(const std::vector<std::string> &Arguments) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, {}, 30);
}

class SourcePatchObject : public testing::Test {
protected:
  std::filesystem::path Directory, Original, Output, Object;
  neverd_session_t Session = nullptr;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  void SetUp() override {
    if (std::string(NEVERD_TEST_CLANG).empty())
      GTEST_SKIP() << "cross Clang and LLD required for object fixtures";
    auto LLD =
        std::filesystem::path(NEVERD_TEST_CLANG).parent_path() / "ld.lld";
    if (!std::filesystem::exists(LLD)) {
      auto Program = llvm::sys::findProgramByName("ld.lld");
      if (Program)
        LLD = *Program;
    }
    if (!std::filesystem::exists(LLD))
      GTEST_SKIP() << "matching ld.lld is unavailable";
    llvm::SmallString<256> Temp;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-object", Temp));
    Directory = Temp.str().str();
    Original = Directory / "original";
    Output = Directory / "patched";
    Object = Directory / "replacement.o";
    const auto Source = Directory / "original.c";
    std::ofstream(Source) << R"(
__attribute__((noinline)) int target(int x) { volatile int y=x; return y+1; }
void _start(void) {
  int value = target(2);
  __asm__ volatile("syscall" : : "a"(60L), "D"((long)value) : "rcx", "r11", "memory");
  __builtin_unreachable();
})";
    ASSERT_EQ(run({NEVERD_TEST_CLANG, "--target=x86_64-unknown-linux-gnu",
                   "-fuse-ld=" + LLD.string(), "-O0", "-nostdlib", "-static",
                   "-fno-pie", "-Wl,--eh-frame-hdr", Source.string(), "-o",
                   Original.string()}),
              0);
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    for (int I = 0; I < neverd_func_count(Session); ++I)
      if (take(neverd_func_name(Session, I)) == "target")
        Mapping.original_va = neverd_func_entry(Session, I);
    ASSERT_NE(Mapping.original_va, 0u);
    Mapping.symbol = "replacement";
    Options.struct_size = sizeof(Options);
    Options.input_kind = NEVERD_SOURCE_PATCH_OBJECT;
    Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
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
  std::string error() { return take(neverd_last_error(Session)); }
  void compile(const char *Source = "int replacement(int x) { return x+40; }") {
    const auto Path = Directory / "replacement.c";
    std::ofstream(Path) << Source;
    ASSERT_EQ(
        run({NEVERD_TEST_CLANG, "--target=x86_64-unknown-linux-gnu", "-O0",
             "-fPIC", "-fno-lto", "-c", Path.string(), "-o", Object.string()}),
        0);
  }
  int patch(const std::string &Bytes) {
    return neverd_patch_source(Session, Bytes.data(), Bytes.size(), &Options,
                               Output.string().c_str());
  }
};

TEST_F(SourcePatchObject, InstallsExactMachineCodeWithoutRecompilation) {
  compile();
  const std::string Before = read(Original), Bytes = read(Object);
  ASSERT_EQ(patch(Bytes), 1) << error();
  EXPECT_EQ(read(Original), Before);
  EXPECT_EQ(neverd_patch_trampoline_count(Session), 1);
  const auto ReportText = take(neverd_patch_source_report(Session));
  auto Report = llvm::json::parse(ReportText);
  ASSERT_TRUE(bool(Report));
  ASSERT_EQ(Report->getAsObject()->getBoolean("success"), true);
  auto Input = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(Input));
  const std::string Patched = read(Output);
  auto Result = llvm::object::ELFFile<ELF>::create(Patched);
  ASSERT_TRUE(bool(Result));
  auto InSections = Input->sections(), OutSections = Result->sections();
  ASSERT_TRUE(bool(InSections));
  ASSERT_TRUE(bool(OutSections));
  bool Compared = false;
  for (const auto &Section : *InSections) {
    auto Name = Input->getSectionName(Section);
    ASSERT_TRUE(bool(Name));
    if (*Name != ".text")
      continue;
    auto Code = Input->getSectionContents(Section);
    ASSERT_TRUE(bool(Code));
    for (const auto &Installed : *OutSections) {
      auto OutName = Result->getSectionName(Installed);
      ASSERT_TRUE(bool(OutName));
      if (*OutName != ".neverd.text")
        continue;
      auto NewCode = Result->getSectionContents(Installed);
      ASSERT_TRUE(bool(NewCode));
      EXPECT_EQ(*Code, *NewCode);
      Compared = true;
    }
  }
  EXPECT_TRUE(Compared);
#if defined(__linux__) && defined(__x86_64__)
  EXPECT_EQ(run({Original.string()}), 3);
  EXPECT_EQ(run({Output.string()}), 42);
#endif
}

TEST_F(SourcePatchObject, NativeCompilerAndPrecompiledInputShareTheInstaller) {
  Options.input_kind = NEVERD_SOURCE_PATCH_SOURCE;
  Options.compiler_path = NEVERD_TEST_CLANG;
  ASSERT_EQ(patch("int replacement(int x) { return x+40; }"), 1) << error();
  const auto First = read(Output);
  Output = Directory / "precompiled";
  const auto Path = Directory / "same.c";
  std::ofstream(Path) << "int replacement(int x) { return x+40; }";
  ASSERT_EQ(run({NEVERD_TEST_CLANG, "--target=x86_64-unknown-linux-gnu", "-O2",
                 "-fno-discard-value-names", "-fno-lto", "-fPIC", "-c",
                 Path.string(), "-o", Object.string()}),
            0);
  Options.input_kind = NEVERD_SOURCE_PATCH_OBJECT;
  ASSERT_EQ(patch(read(Object)), 1) << error();
  EXPECT_EQ(read(Output), First);
}

TEST_F(SourcePatchObject, SourceCompilerCannotExpandResponseFileValues) {
  const auto Response = Directory / "extra arguments.rsp";
  std::ofstream(Response) << "unused-directory -DBASE=37\n";
  const std::string ResponseArgument = "@" + Response.string();
  const char *Arguments[] = {"-I", ResponseArgument.c_str()};
  Options.input_kind = NEVERD_SOURCE_PATCH_SOURCE;
  Options.compiler_path = NEVERD_TEST_CLANG;
  Options.compiler_args = Arguments;
  Options.compiler_arg_count = 2;
  EXPECT_EQ(patch("int replacement(int x) { return x+BASE; }"), 0);
  EXPECT_NE(error().find("response files"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, NeverCEntryTableCannotLoseItsLoadTimeRelocations) {
  const char *NeverC = std::getenv("NEVERD_TEST_NEVERC");
  if (!NeverC || !*NeverC)
    GTEST_SKIP() << "set NEVERD_TEST_NEVERC for the optional native producer";
  const auto Source = Directory / "padded.c";
  std::ofstream(Source)
      << "__attribute__((noinline)) int helper(int x) { return x+1; } "
         "int replacement(int x) { return helper(x)+39; }";
  ASSERT_EQ(run({NeverC, "--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC",
                 "-fno-lto", "-fpatchable-function-entry=16", "-c",
                 Source.string(), "-o", Object.string()}),
            0);
  const auto Bytes = read(Object);
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(File));
  auto Sections = File->sections();
  ASSERT_TRUE(bool(Sections));
  // This static fixture has no dynamic relocation table. Drop neither the
  // link-order entry table nor its pointers just to make installation succeed.
  EXPECT_EQ(patch(Bytes), 0);
  EXPECT_NE(error().find("PT_DYNAMIC"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, InvalidOrForeignObjectsCannotPublish) {
  compile();
  const auto Bytes = read(Object);
  for (auto Invalid : {std::string("BC\300\336"), Bytes.substr(0, 40), Bytes}) {
    if (Invalid.size() == Bytes.size())
      neverd::writeLE<uint16_t>(reinterpret_cast<uint8_t *>(Invalid.data()) +
                                    18,
                                llvm::ELF::EM_AARCH64);
    EXPECT_EQ(patch(Invalid), 0);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
}

TEST_F(SourcePatchObject, OutOfBoundsRelocationCannotPublish) {
  compile("extern int external(int); int replacement(int x) { return "
          "external(x); }");
  auto Bytes = read(Object);
  auto File = llvm::object::ELFFile<ELF>::create(Bytes);
  ASSERT_TRUE(bool(File));
  auto Sections = File->sections();
  ASSERT_TRUE(bool(Sections));
  bool Changed = false;
  for (const auto &Section : *Sections) {
    if (Section.sh_type == llvm::ELF::SHT_RELA && Section.sh_size) {
      neverd::writeLE<uint64_t>(reinterpret_cast<uint8_t *>(Bytes.data()) +
                                    Section.sh_offset,
                                UINT64_MAX - 1);
      Changed = true;
      break;
    }
  }
  ASSERT_TRUE(Changed);
  EXPECT_EQ(patch(Bytes), 0);
  EXPECT_NE(error().find("relocation exceeds"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, MissingEntryAndUnsupportedTLSCannotPublish) {
  compile();
  Mapping.symbol = "missing";
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
  Mapping.symbol = "replacement";
  compile(
      "_Thread_local int value; int replacement(int x) { return ++value+x; }");
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, LegacyInitializationCannotBeSilentlyDropped) {
  compile("__attribute__((section(\".init\"),used)) void initialize(void) {} "
          "int replacement(int x) { return x+40; }");
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_NE(error().find("initialization/finalization"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

TEST_F(SourcePatchObject, CallableRelocationCannotResolveToData) {
  compile(
      "__asm__(\".data\\n.type external,@object\\nexternal: .long 42\\n\"); "
      "extern int external(int); "
      "int replacement(int x) { return external(x); }");
  EXPECT_EQ(patch(read(Object)), 0);
  EXPECT_NE(error().find("callable symbol does not name code"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}

} // namespace
