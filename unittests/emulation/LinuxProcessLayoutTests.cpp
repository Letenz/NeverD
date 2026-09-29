//===- LinuxProcessLayoutTests.cpp - Loader facts and guest image policy -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/LinuxProcess.h"

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/ELFLoader.h"

#include "llvm/BinaryFormat/ELF.h"

namespace neverd::emulation {
namespace {
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_TEXT

class LinuxProcessLayout : public testing::Test {
protected:
  BinaryImage Image;
  void SetUp() override {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << MissingTools;
#else
    ELFLoader Loader;
    auto Loaded = Loader.load(
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) / ARMFile);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Image = std::move(*Loaded);
#endif
  }
  void rejects(const BinaryImage &Invalid) {
    auto Layout = linux_model::processLayout(Invalid);
    EXPECT_FALSE(bool(Layout));
    llvm::consumeError(Layout.takeError());
  }
};

TEST_F(LinuxProcessLayout, KeepsLoaderProgramHeadersAndRequiresMappedTable) {
  ASSERT_TRUE(Image.ELFMetadata);
  auto Layout = linux_model::processLayout(Image);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  const auto &Metadata = *Image.ELFMetadata;
  EXPECT_EQ(Metadata.Type, llvm::ELF::ET_EXEC);
  EXPECT_EQ(Metadata.ProgramHeaderEntrySize, sizeof(llvm::ELF::Elf64_Phdr));
  EXPECT_FALSE(Metadata.ProgramHeaders.empty());
  EXPECT_TRUE(Image.getSegmentFor(Layout->ProgramHeaderAddress));
  for (auto &Header : Image.ELFMetadata->ProgramHeaders)
    if (Header.Type == llvm::ELF::PT_LOAD && Header.FileOffset == 0)
      Header.FileSize = 1;
  rejects(Image);
}

TEST_F(LinuxProcessLayout,
       RejectsUnimplementedLoaderAndTLSContractsExplicitly) {
  for (uint32_t Type :
       {llvm::ELF::PT_INTERP, llvm::ELF::PT_DYNAMIC, llvm::ELF::PT_TLS}) {
    auto Invalid = Image;
    Invalid.ELFMetadata->ProgramHeaders.front().Type = Type;
    auto Layout = linux_model::processLayout(Invalid);
    EXPECT_FALSE(bool(Layout));
    EXPECT_EQ(llvm::toString(Layout.takeError()), linux_model::Dynamic);
  }
  Image.ELFMetadata->Type = llvm::ELF::ET_DYN;
  rejects(Image);
}

TEST_F(LinuxProcessLayout, RejectsContradictoryABIAndSegmentLayoutFacts) {
  auto Invalid = Image;
  Invalid.ELFMetadata->OSABI = llvm::ELF::ELFOSABI_FREEBSD;
  rejects(Invalid);
  Invalid = Image;
  Invalid.ELFMetadata->ABIVersion = 1;
  rejects(Invalid);
  Invalid = Image;
  Invalid.ELFMetadata->Flags = 1;
  rejects(Invalid);
  for (auto Mutate :
       {+[](ELFProgramHeader &H) { H.Alignment = 3; },
        +[](ELFProgramHeader &H) { ++H.FileOffset; },
        +[](ELFProgramHeader &H) { H.FileSize = H.MemorySize + 1; },
        +[](ELFProgramHeader &H) { H.MemorySize = UINT64_MAX; },
        +[](ELFProgramHeader &H) { H.VirtualAddress = 0; }}) {
    Invalid = Image;
    auto I =
        llvm::find_if(Invalid.ELFMetadata->ProgramHeaders, [](const auto &H) {
          return H.Type == llvm::ELF::PT_LOAD;
        });
    ASSERT_NE(I, Invalid.ELFMetadata->ProgramHeaders.end());
    Mutate(*I);
    rejects(Invalid);
  }
}
} // namespace
} // namespace neverd::emulation
