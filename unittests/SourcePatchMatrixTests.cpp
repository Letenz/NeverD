//===- SourcePatchMatrixTests.cpp - Source patch target execution matrix
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cross-target source compilation, publication and isolated CPU execution.
/// This is not an operating-system loader or exception-runtime test.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Loader.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <unicorn/unicorn.h>

namespace {
using namespace neverd;
struct Profile {
  Arch Architecture;
  BinaryFormat Format;
  const char *Name;
};
const Profile Profiles[] = {
    {Arch::X86, BinaryFormat::ELF, "ELF_X86"},
    {Arch::X64, BinaryFormat::ELF, "ELF_X64"},
    {Arch::ARM, BinaryFormat::ELF, "ELF_ARM32"},
    {Arch::AArch64, BinaryFormat::ELF, "ELF_ARM64"},
    {Arch::X86, BinaryFormat::COFF, "PE_X86"},
    {Arch::X64, BinaryFormat::COFF, "PE_X64"},
    {Arch::ARM, BinaryFormat::COFF, "PE_ARM32"},
    {Arch::AArch64, BinaryFormat::COFF, "PE_ARM64"},
    {Arch::X86, BinaryFormat::MachO, "MachO_X86"},
    {Arch::X64, BinaryFormat::MachO, "MachO_X64"},
    {Arch::ARM, BinaryFormat::MachO, "MachO_ARM32"},
    {Arch::AArch64, BinaryFormat::MachO, "MachO_ARM64"},
};
std::string take(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}
std::string read(const std::filesystem::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(Input), {}};
}
void put(std::string &Bytes, size_t At, uint64_t Value, unsigned Width) {
  for (unsigned I = 0; I < Width; ++I)
    Bytes.at(At + I) = char(Value >> (8 * I));
}
// Modern ld64 cannot link these legacy 32-bit Mach-O targets. An unsigned
// image with an ordinary ABI entry lets the refusal tests authenticate the
// input and execute its original bytes without inventing an OS runtime profile.
std::string legacyMachOImage(const Profile &P) {
  std::string B(0x2000, '\0');
  constexpr uint32_t Base = 0x400000, Code = 0x1000;
  constexpr size_t H = 28, S = 56, Sec = 68;
  put(B, 0, 0xfeedface, 4);
  put(B, 4, P.Architecture == Arch::ARM ? 12 : 7, 4);
  put(B, 8, P.Architecture == Arch::ARM ? 9 : 3, 4);
  put(B, 12, 2, 4);
  put(B, 16, 2, 4);
  put(B, 20, S + Sec + 24, 4);
  put(B, H, 1, 4);
  put(B, H + 4, S + Sec, 4);
  B.replace(H + 8, 6, "__TEXT");
  put(B, H + 24, Base, 4);
  put(B, H + 28, B.size(), 4);
  put(B, H + 36, B.size(), 4);
  put(B, H + 40, 5, 4);
  put(B, H + 44, 5, 4);
  put(B, H + 48, 1, 4);
  const size_t T = H + S;
  B.replace(T, 6, "__text");
  B.replace(T + 16, 6, "__TEXT");
  put(B, T + 32, Base + Code, 4);
  put(B, T + 36, 4096, 4);
  put(B, T + 40, Code, 4);
  put(B, T + 44, 4, 4);
  put(B, T + 56, 0x80000400, 4);
  const size_t E = H + S + Sec;
  put(B, E, 0x80000028, 4);
  put(B, E + 4, 24, 4);
  put(B, E + 8, Code, 8);
  const std::string Instructions =
      P.Architecture == Arch::ARM
          ? std::string("\x01\x00\x80\xe2\x1e\xff\x2f\xe1", 8)
          : std::string("\x8b\x44\x24\x04\x40\xc3", 6);
  B.replace(Code, Instructions.size(), Instructions);
  return B;
}

// Map image bytes and invoke an ordinary ABI function. The original entry is
// used after installation, so this verifies the entry branch as well as
// codegen.
void execute(const BinaryImage &Image, const Profile &P, int Argument,
             int Expected) {
  uc_engine *UC = nullptr;
  const auto CPU = P.Architecture == Arch::ARM       ? UC_ARCH_ARM
                   : P.Architecture == Arch::AArch64 ? UC_ARCH_ARM64
                                                     : UC_ARCH_X86;
  const auto Mode =
      P.Architecture == Arch::X86   ? UC_MODE_32
      : P.Architecture == Arch::X64 ? UC_MODE_64
      : P.Architecture == Arch::ARM && P.Format == BinaryFormat::COFF
          ? UC_MODE_THUMB
          : UC_MODE_ARM;
  ASSERT_EQ(uc_open(CPU, Mode, &UC), UC_ERR_OK);
  auto Close = llvm::scope_exit([&] { uc_close(UC); });
  std::set<uint64_t> Pages;
  for (const auto &Segment : Image.Segments) {
    if (Segment.Data.empty())
      continue;
    ASSERT_LT(Segment.Data.size(), 16u * 1024 * 1024);
    for (uint64_t Page = Segment.VA & ~uint64_t(4095);
         Page < Segment.VA + Segment.Data.size(); Page += 4096)
      if (Pages.insert(Page).second)
        ASSERT_EQ(uc_mem_map(UC, Page, 4096, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(
        uc_mem_write(UC, Segment.VA, Segment.Data.data(), Segment.Data.size()),
        UC_ERR_OK);
  }
  constexpr uint64_t Stack = 0x70000000, Return = 0x7fff0000;
  ASSERT_EQ(uc_mem_map(UC, Stack, 0x10000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(UC, Return, 4096, UC_PROT_ALL), UC_ERR_OK);
  uint64_t SP = Stack + 0xff00, Arg = uint32_t(Argument), LR = Return;
  int ResultReg;
  switch (P.Architecture) {
  case Arch::X64:
    SP -= 8;
    ASSERT_EQ(uc_mem_write(UC, SP, &LR, 8), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_RSP, &SP), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC,
                           P.Format == BinaryFormat::COFF ? UC_X86_REG_RCX
                                                          : UC_X86_REG_RDI,
                           &Arg),
              UC_ERR_OK);
    ResultReg = UC_X86_REG_RAX;
    break;
  case Arch::X86:
    SP -= 4;
    ASSERT_EQ(uc_mem_write(UC, SP, &LR, 4), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(UC, SP + 4, &Arg, 4), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_X86_REG_ESP, &SP), UC_ERR_OK);
    ResultReg = UC_X86_REG_EAX;
    break;
  case Arch::ARM:
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_SP, &SP), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_LR, &LR), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM_REG_R0, &Arg), UC_ERR_OK);
    ResultReg = UC_ARM_REG_R0;
    break;
  case Arch::AArch64:
    ASSERT_EQ(uc_reg_write(UC, UC_ARM64_REG_SP, &SP), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM64_REG_LR, &LR), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(UC, UC_ARM64_REG_W0, &Arg), UC_ERR_OK);
    ResultReg = UC_ARM64_REG_W0;
    break;
  default:
    llvm_unreachable("matrix architecture");
  }
  const uint64_t Start =
      Image.Entry |
      (P.Architecture == Arch::ARM && P.Format == BinaryFormat::COFF ? 1 : 0);
  ASSERT_EQ(uc_emu_start(UC, Start, Return, 1000000, 10000), UC_ERR_OK);
  uint64_t Value = 0, PC = 0;
  ASSERT_EQ(uc_reg_read(UC, ResultReg, &Value), UC_ERR_OK);
  const int PCReg = P.Architecture == Arch::X64   ? UC_X86_REG_RIP
                    : P.Architecture == Arch::X86 ? UC_X86_REG_EIP
                    : P.Architecture == Arch::ARM ? int(UC_ARM_REG_PC)
                                                  : int(UC_ARM64_REG_PC);
  ASSERT_EQ(uc_reg_read(UC, PCReg, &PC), UC_ERR_OK);
  EXPECT_EQ(PC, Return)
      << "must return, not merely exhaust the instruction budget";
  EXPECT_EQ(int32_t(Value), Expected) << "argument " << Argument;
}

int run(const std::vector<std::string> &Arguments) {
  std::vector<llvm::StringRef> Args(Arguments.begin(), Arguments.end());
  return llvm::sys::ExecuteAndWait(Args.front(), Args, std::nullopt, {}, 30);
}
std::string linker(const char *Name) {
  auto Program = llvm::sys::findProgramByName(Name);
  if (Program)
    return *Program;
  const auto Candidate =
      std::filesystem::path(NEVERD_TEST_CLANG).parent_path() / Name;
  return std::filesystem::exists(Candidate) ? Candidate.string() : "";
}

const char *refusalReason(const Profile &P) {
  if (P.Format == BinaryFormat::COFF && P.Architecture != Arch::X64)
    return "requires PE x64";
  if (P.Format == BinaryFormat::ELF && P.Architecture == Arch::ARM)
    return "EHABI registration contract";
  if (P.Format == BinaryFormat::MachO && P.Architecture == Arch::ARM)
    return "verified watchOS frontend ABI";
  if (P.Format == BinaryFormat::MachO && P.Architecture == Arch::X86)
    return "verified loader/unwind contract";
  return nullptr;
}

class SourcePatchMatrix : public testing::TestWithParam<Profile> {
protected:
  std::filesystem::path Directory, Original, Output;
  BinaryImage Image;
  std::string OriginalBytes;
  neverd_session_t Session = nullptr;
  neverd_source_patch_options_v1 Options{};
  neverd_source_patch_mapping_v1 Mapping{};
  void SetUp() override {
    ASSERT_FALSE(std::string(NEVERD_TEST_CLANG).empty());
    llvm::SmallString<256> Temp;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-source-matrix", Temp));
    Directory = Temp.str().str();
    Original = Directory / "original";
    Output = Directory / "patched";
    const auto P = GetParam();
    if (P.Format == BinaryFormat::MachO &&
        (P.Architecture == Arch::X86 || P.Architecture == Arch::ARM)) {
      // Modern ld64 no longer links these legacy Mach-O ABIs. Synthetic
      // images let us verify an explicit refusal without claiming OS support.
      const auto Bytes = legacyMachOImage(P);
      std::ofstream(Original, std::ios::binary)
          .write(Bytes.data(), Bytes.size());
    } else {
      const auto Source = Directory / "original.c",
                 Object = Directory / "original.o";
      std::ofstream(Source)
          << (P.Format == BinaryFormat::COFF ? "__declspec(dllexport) " : "")
          << "__attribute__((noinline)) int target(int x) { volatile int y=x; "
             "return y+1; }";
      ASSERT_EQ(run({NEVERD_TEST_CLANG,
                     std::string("--target=") +
                         llvmCodegenTriple(P.Architecture, P.Format),
                     "-O0", "-funwind-tables", "-fno-pie", "-c",
                     Source.string(), "-o", Object.string()}),
                0);
      const auto Linker = linker(P.Format == BinaryFormat::ELF    ? "ld.lld"
                                 : P.Format == BinaryFormat::COFF ? "lld-link"
                                                                  : "ld64.lld");
      ASSERT_FALSE(Linker.empty())
          << "cross LLD is required for the complete matrix";
      if (P.Format == BinaryFormat::ELF) {
        ASSERT_EQ(run({Linker, "--eh-frame-hdr", "-e", "target",
                       Object.string(), "-o", Original.string()}),
                  0);
      } else if (P.Format == BinaryFormat::COFF) {
        ASSERT_EQ(run({Linker, Object.string(), "/out:" + Original.string(),
                       "/entry:target", "/subsystem:console", "/nodefaultlib"}),
                  0);
      } else {
        ASSERT_EQ(run({Linker, "-arch",
                       P.Architecture == Arch::X64 ? "x86_64" : "arm64",
                       "-platform_version", "macos", "14.0", "14.0", "-e",
                       "_target", "-headerpad", "0x800", "-no_adhoc_codesign",
                       Object.string(), "-o", Original.string()}),
                  0);
      }
    }
    auto Reader = Loader::create(Original);
    ASSERT_NE(Reader, nullptr);
    auto Loaded = Reader->load(Original);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
    OriginalBytes = read(Original);
    ASSERT_EQ(Image.Arch, GetParam().Architecture);
    ASSERT_EQ(Image.Format, GetParam().Format);
    execute(Image, GetParam(), 7, 8);
    ASSERT_FALSE(HasFatalFailure());
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    ASSERT_EQ(neverd_session_load(Session, Original.string().c_str()), 1)
        << error();
    Mapping = {"replacement", Image.Entry};
    Options.struct_size = sizeof(Options);
    Options.compiler_path = NEVERD_TEST_CLANG;
    Options.mappings = &Mapping;
    Options.mapping_count = 1;
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Ignored;
    if (!Directory.empty())
      std::filesystem::remove_all(Directory, Ignored);
  }
  std::string error() { return take(neverd_last_error(Session)); }
  int patch() {
    constexpr const char *Source =
        "__attribute__((noinline)) static int helper(int x) { volatile int "
        "y=x; return y*3; } int replacement(int x) { if(x<0) return -7; return "
        "helper(x)+40; }";
    return neverd_patch_source(Session, Source, std::strlen(Source), &Options,
                               Output.string().c_str());
  }
};

TEST_P(SourcePatchMatrix, CompilesInstallsAndExecutesOrExplicitlyRefuses) {
  const auto P = GetParam();
  const int Result = patch();
  if (const char *Reason = refusalReason(P)) {
    EXPECT_EQ(Result, 0);
    EXPECT_NE(error().find(Reason), std::string::npos);
    auto Report = llvm::json::parse(take(neverd_patch_source_report(Session)));
    ASSERT_TRUE(bool(Report));
    EXPECT_EQ(Report->getAsObject()->getBoolean("success"), false);
    EXPECT_NE(Report->getAsObject()->getString("error")->find(Reason),
              llvm::StringRef::npos);
    EXPECT_FALSE(std::filesystem::exists(Output));
    EXPECT_EQ(neverd_patch_trampoline_count(Session), 0);
    return;
  }
  ASSERT_EQ(Result, 1) << error();
  auto Report = llvm::json::parse(take(neverd_patch_source_report(Session)));
  ASSERT_TRUE(bool(Report));
  const auto *Installed = Report->getAsObject()->getArray("installed");
  ASSERT_NE(Installed, nullptr);
  ASSERT_EQ(Installed->size(), 1u);
  EXPECT_EQ(Installed->front().getAsObject()->getInteger("original_va"),
            Image.Entry);
  auto Reader = Loader::create(Output);
  ASSERT_NE(Reader, nullptr);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {-1, 0, 1, 7, 25})
    execute(*Reloaded, P, X, X < 0 ? -7 : X * 3 + 40);
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_P(SourcePatchMatrix, SuppliedIRUsesTheSameInstallationContract) {
  const auto P = GetParam();
  Options.input_kind = NEVERD_SOURCE_PATCH_LLVM_IR;
  const std::string IR = std::string("target triple = \"") +
                         llvmCodegenTriple(P.Architecture, P.Format) + "\"\n" +
                         "target datalayout = \"e-p:" +
                         std::to_string(Image.getPointerSize() * 8) + ":" +
                         std::to_string(Image.getPointerSize() * 8) + "\"\n" +
                         "define i32 @replacement(i32 %x) uwtable {\n"
                         "  %value = add i32 %x, 40\n  ret i32 %value\n}\n";
  const int Result = neverd_patch_source(Session, IR.data(), IR.size(),
                                         &Options, Output.string().c_str());
  if (const char *Reason = refusalReason(P)) {
    EXPECT_EQ(Result, 0);
    EXPECT_NE(error().find(Reason), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(Output));
    return;
  }
  ASSERT_EQ(Result, 1) << error();
  auto Reader = Loader::create(Output);
  ASSERT_NE(Reader, nullptr);
  auto Reloaded = Reader->load(Output);
  ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
  for (int X : {-1, 0, 7, 25})
    execute(*Reloaded, P, X, X + 40);
  EXPECT_EQ(read(Original), OriginalBytes);
}

TEST_P(SourcePatchMatrix, NativeObjectProfileCannotFallBackToIR) {
  if (GetParam().Format == BinaryFormat::ELF &&
      GetParam().Architecture == Arch::X64) {
    Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
    ASSERT_EQ(patch(), 1) << error();
    auto Reader = Loader::create(Output);
    ASSERT_NE(Reader, nullptr);
    auto Reloaded = Reader->load(Output);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    execute(*Reloaded, GetParam(), 7, 61);
    return;
  }
  Options.artifact = NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("requires ELF x64"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}
TEST_P(SourcePatchMatrix, DyncodeCannotPublishAnOrdinaryCompilation) {
  Options.mode = NEVERD_SOURCE_PATCH_DYNCODE;
  EXPECT_EQ(patch(), 0);
  EXPECT_NE(error().find("dyncode"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(Output));
}
INSTANTIATE_TEST_SUITE_P(AllTargets, SourcePatchMatrix,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &Info) {
                           return Info.param.Name;
                         });
} // namespace
