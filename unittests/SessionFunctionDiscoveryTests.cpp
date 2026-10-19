//===- SessionFunctionDiscoveryTests.cpp - Recovered function extents
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/FileSystem.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
std::string takeString(const char *Text) {
  const std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

std::string makeNativeELF(bool AArch64, uint64_t Base = 0x400000,
                          std::string_view TrailingCode = {},
                          std::string_view Body = {}) {
  using ELF = llvm::object::ELF64LE;
  using namespace llvm::ELF;
  std::string Code = !Body.empty() ? std::string(Body)
                     : AArch64
                         ? std::string("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
                         : std::string("\xb8\x07\x00\x00\x00\xc3", 6);
  Code += TrailingCode;
  const size_t CodeOffset = sizeof(ELF::Ehdr) + sizeof(ELF::Phdr);
  std::string Bytes(CodeOffset + Code.size(), '\0');
  ELF::Ehdr Header{};
  std::memcpy(Header.e_ident, ElfMagic, sizeof(ElfMagic) - 1);
  Header.e_ident[EI_CLASS] = ELFCLASS64;
  Header.e_ident[EI_DATA] = ELFDATA2LSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_type = ET_EXEC;
  Header.e_machine = AArch64 ? EM_AARCH64 : EM_X86_64;
  Header.e_version = EV_CURRENT;
  Header.e_entry = Base + CodeOffset;
  Header.e_phoff = sizeof(ELF::Ehdr);
  Header.e_ehsize = sizeof(ELF::Ehdr);
  Header.e_phentsize = sizeof(ELF::Phdr);
  Header.e_phnum = 1;
  Header.e_shentsize = sizeof(ELF::Shdr);
  ELF::Phdr Segment{};
  Segment.p_type = PT_LOAD;
  Segment.p_flags = PF_R | PF_X;
  Segment.p_vaddr = Base;
  Segment.p_paddr = Base;
  Segment.p_filesz = Bytes.size();
  Segment.p_memsz = Bytes.size();
  Segment.p_align = 0x1000;
  std::memcpy(Bytes.data(), &Header, sizeof(Header));
  std::memcpy(Bytes.data() + sizeof(Header), &Segment, sizeof(Segment));
  Bytes.replace(CodeOffset, Code.size(), Code);
  return Bytes;
}

class SessionFunctionDiscovery : public ::testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-discovery", Path));
    Directory = Path.c_str();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Error;
    std::filesystem::remove_all(Directory, Error);
  }
  std::string write(std::string_view Name, std::string_view Bytes) {
    const auto File = Directory / Name;
    std::ofstream Stream(File, std::ios::binary);
    Stream.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
    return File.string();
  }
};

TEST_F(SessionFunctionDiscovery,
       NativeDiscoveryBeforeAnalysisPublishesRecoveredExtents) {
  for (bool AArch64 : {false, true}) {
    for (bool Lazy : {false, true}) {
      for (neverd_va_t Base : {0x400000ULL, 0xffff800000400000ULL}) {
        SCOPED_TRACE(AArch64);
        SCOPED_TRACE(Lazy);
        SCOPED_TRACE(Base);
        const std::string Path =
            write("discovered.elf", makeNativeELF(AArch64, Base));
        ASSERT_EQ(neverd_session_load(Session, Path.c_str()), 1);
        const auto Entry = neverd_session_entry_addr(Session);
        ASSERT_EQ(neverd_session_discover_functions(Session), 1);
        ASSERT_EQ(neverd_func_size(Session, 0), 0);
        const auto Name = takeString(neverd_func_name(Session, 0));
        if (Lazy)
          ASSERT_FALSE(takeString(neverd_ir_low(Session, Entry)).empty());
        else
          ASSERT_EQ(neverd_session_analyze(Session), 1);
        EXPECT_EQ(neverd_func_entry(Session, 0), Entry);
        EXPECT_EQ(neverd_func_size(Session, 0), AArch64 ? 8 : 6);
        EXPECT_EQ(takeString(neverd_func_name(Session, 0)), Name);
        ASSERT_FALSE(takeString(neverd_ir_med(Session, Entry)).empty());
        ASSERT_EQ(neverd_session_analyze(Session), 1);
        EXPECT_EQ(neverd_func_size(Session, 0), AArch64 ? 8 : 6);
      }
    }
  }
}

TEST_F(SessionFunctionDiscovery,
       NativeRecoveryKeepsKnownExtentsAndDeletedEntries) {
  for (bool MissingEntry : {false, true}) {
    auto &State = *reinterpret_cast<neverd::sdk::Session *>(Session);
    State.Loaded = true;
    State.Img.Arch = neverd::Arch::X64;
    State.PipeRan = true;
    State.PipeResult.Success = true;
    State.NativeFunctionsSynchronized = false;
    State.Functions = {{0x1000, 0, "discovered"}, {0x2000, 23, "stated"}};
    State.functionListChanged();
    State.FunctionEdits[0x4000] = false;
    State.PipeResult.LowFuncs.clear();
    for (auto Entry : {0x1000, 0x2000, 0x4000}) {
      neverd::LowFunc F;
      F.Entry = Entry;
      F.OriginalSize = 8;
      State.PipeResult.LowFuncs.push_back(std::move(F));
    }
    if (MissingEntry) {
      neverd::LowFunc F;
      F.Entry = 0x3000;
      F.OriginalSize = 12;
      State.PipeResult.LowFuncs.push_back(std::move(F));
    }
    ASSERT_EQ(neverd_func_count(Session), MissingEntry ? 3 : 2);
    EXPECT_EQ(neverd_func_size(Session, 0), 8);
    EXPECT_EQ(neverd_func_size(Session, 1), 23);
    EXPECT_EQ(neverd_func_find_by_addr(Session, 0x4000), -1);
    if (MissingEntry)
      EXPECT_EQ(neverd_func_size(Session, 2), 12);
  }
}

} // namespace
