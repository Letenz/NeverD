//===- LinuxProcessLayoutTests.cpp - Loader facts and guest image policy -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/LinuxProcess.h"

#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramMetadata.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_TEXT
namespace tls_fixture {
#define NEVERD_TLS_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxTLSCases.def"
#undef NEVERD_TLS_TEXT
} // namespace tls_fixture
namespace pie_fixture {
#define NEVERD_PIE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PIE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxPIECases.def"
#undef NEVERD_PIE_VALUE
#undef NEVERD_PIE_TEXT
} // namespace pie_fixture

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

TEST_F(LinuxProcessLayout, RejectsUnimplementedDynamicLinkContractsExplicitly) {
  Image.ELFMetadata->ProgramHeaders.front().Type = llvm::ELF::PT_INTERP;
  auto Layout = linux_model::processLayout(Image);
  EXPECT_FALSE(bool(Layout));
  EXPECT_EQ(llvm::toString(Layout.takeError()), linux_model::Dynamic);
}

TEST_F(LinuxProcessLayout, StaticPIEUsesLoaderFactsWithoutDependingOnSections) {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  ELFLoader Loader;
  auto Loaded = Loader.load(std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
                            pie_fixture::ARMFile);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  ASSERT_EQ(Image.ELFMetadata->Type, llvm::ELF::ET_DYN);
  Image.Sections.clear();
  Image.DynInfo = {};
  auto Layout = linux_model::processLayout(Image);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  EXPECT_EQ(Layout->LoadBias, linux_model::StaticPIEBias);
  EXPECT_GE(Layout->ProgramHeaderAddress, Layout->LoadBias);
  auto Aligned = Image;
  auto FirstLoad =
      llvm::find_if(Aligned.ELFMetadata->ProgramHeaders,
                    [](const auto &H) { return H.Type == llvm::ELF::PT_LOAD; });
  ASSERT_NE(FirstLoad, Aligned.ELFMetadata->ProgramHeaders.end());
  ASSERT_EQ(FirstLoad->VirtualAddress, 0u);
  ASSERT_EQ(FirstLoad->FileOffset, 0u);
  FirstLoad->Alignment = 2 * linux_model::StaticPIEBias;
  auto AlignedLayout = linux_model::processLayout(Aligned);
  ASSERT_TRUE(bool(AlignedLayout)) << llvm::toString(AlignedLayout.takeError());
  EXPECT_EQ(AlignedLayout->LoadBias, FirstLoad->Alignment);
  FirstLoad->Alignment = linux_model::UserLimitARM64;
  rejects(Aligned);
  auto Entries = readELFProgramDynamicTable(Image);
  ASSERT_TRUE(bool(Entries)) << llvm::toString(Entries.takeError());
  EXPECT_FALSE(Entries->empty());
  auto Table =
      llvm::find_if(Image.ELFMetadata->ProgramHeaders, [](const auto &H) {
        return H.Type == llvm::ELF::PT_DYNAMIC;
      });
  ASSERT_NE(Table, Image.ELFMetadata->ProgramHeaders.end());
  const size_t Index = Table - Image.ELFMetadata->ProgramHeaders.begin();
  auto Dependency = Image;
  llvm::support::endian::write64le(Dependency.Raw.data() + Table->FileOffset,
                                   llvm::ELF::DT_NEEDED);
  auto Unsupported = linux_model::processLayout(Dependency);
  EXPECT_FALSE(bool(Unsupported));
  EXPECT_EQ(llvm::toString(Unsupported.takeError()), linux_model::Dynamic);
  for (auto Mutate :
       {+[](ELFProgramHeader &H) { H.FileSize = 0; },
        +[](ELFProgramHeader &H) { --H.FileSize; },
        +[](ELFProgramHeader &H) { H.FileOffset = UINT64_MAX; },
        +[](ELFProgramHeader &H) { H.VirtualAddress = UINT64_MAX; }}) {
    auto Invalid = Image;
    Mutate(Invalid.ELFMetadata->ProgramHeaders[Index]);
    rejects(Invalid);
  }
  auto Unterminated = Image;
  for (uint64_t Offset = 0; Offset < Table->FileSize;
       Offset += sizeof(llvm::ELF::Elf64_Dyn))
    llvm::support::endian::write64le(Unterminated.Raw.data() +
                                         Table->FileOffset + Offset,
                                     llvm::ELF::DT_DEBUG);
  rejects(Unterminated);
  auto Duplicate = Image;
  auto Stack =
      llvm::find_if(Duplicate.ELFMetadata->ProgramHeaders, [](const auto &H) {
        return H.Type == llvm::ELF::PT_GNU_STACK;
      });
  ASSERT_NE(Stack, Duplicate.ELFMetadata->ProgramHeaders.end());
  *Stack = *Table;
  rejects(Duplicate);
  auto Overflow = Image;
  Overflow.Entry = UINT64_MAX;
  rejects(Overflow);
#endif
}

TEST_F(LinuxProcessLayout, DynamicTableDecoderPreservesBothELFWordWidths) {
  for (bool Wide : {false, true}) {
    auto Input = Image;
    Input.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
    Input.Raw[llvm::ELF::EI_CLASS] =
        Wide ? llvm::ELF::ELFCLASS64 : llvm::ELF::ELFCLASS32;
    const uint64_t Width =
        Wide ? sizeof(llvm::ELF::Elf64_Dyn) : sizeof(llvm::ELF::Elf32_Dyn);
    auto &Header = Input.ELFMetadata->ProgramHeaders.front();
    Header.Type = llvm::ELF::PT_DYNAMIC;
    Header.FileSize = Header.MemorySize = 2 * Width;
    ASSERT_LE(Header.FileOffset + Header.FileSize, Input.Raw.size());
    auto *Bytes = Input.Raw.data() + Header.FileOffset;
    std::fill_n(Bytes, Header.FileSize, 0);
    if (Wide) {
      llvm::support::endian::write64le(Bytes, llvm::ELF::DT_SONAME);
      llvm::support::endian::write64le(Bytes + Width / 2,
                                       pie_fixture::DynamicProbeValue);
    } else {
      llvm::support::endian::write32le(Bytes, llvm::ELF::DT_SONAME);
      llvm::support::endian::write32le(Bytes + Width / 2,
                                       pie_fixture::DynamicProbeValue);
    }
    const auto Entries = llvm::cantFail(readELFProgramDynamicTable(Input));
    ASSERT_EQ(Entries.size(), 1u);
    EXPECT_EQ(Entries[0].Tag, llvm::ELF::DT_SONAME);
    EXPECT_EQ(Entries[0].Value, pie_fixture::DynamicProbeValue);
    Header.FileSize -= Width;
    auto Unterminated = readELFProgramDynamicTable(Input);
    EXPECT_FALSE(bool(Unterminated));
    llvm::consumeError(Unterminated.takeError());
  }
}

TEST_F(LinuxProcessLayout,
       ValidatesTLSFileTemplateWithoutInventingThreadBlocks) {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  ELFLoader Loader;
  auto Loaded = Loader.load(std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
                            tls_fixture::ARMFile);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  Image = std::move(*Loaded);
  auto Layout = linux_model::processLayout(Image);
  ASSERT_TRUE(bool(Layout)) << llvm::toString(Layout.takeError());
  auto Template =
      llvm::find_if(Image.ELFMetadata->ProgramHeaders,
                    [](const auto &H) { return H.Type == llvm::ELF::PT_TLS; });
  ASSERT_NE(Template, Image.ELFMetadata->ProgramHeaders.end());
  EXPECT_GT(Template->FileSize, 0u);
  EXPECT_GT(Template->MemorySize, Template->FileSize);
  const size_t Index = Template - Image.ELFMetadata->ProgramHeaders.begin();
  for (auto Mutate :
       {+[](ELFProgramHeader &H) { H.Alignment = 3; },
        +[](ELFProgramHeader &H) { H.MemorySize = 0; },
        +[](ELFProgramHeader &H) { H.FileSize = H.MemorySize + 1; },
        +[](ELFProgramHeader &H) { H.FileOffset = UINT64_MAX; },
        +[](ELFProgramHeader &H) { H.VirtualAddress = 0; },
        +[](ELFProgramHeader &H) { ++H.FileOffset; }}) {
    auto Invalid = Image;
    Mutate(Invalid.ELFMetadata->ProgramHeaders[Index]);
    rejects(Invalid);
  }
  auto ZeroOnly = Image;
  ZeroOnly.ELFMetadata->ProgramHeaders[Index].FileSize = 0;
  auto ZeroLayout = linux_model::processLayout(ZeroOnly);
  EXPECT_TRUE(bool(ZeroLayout)) << llvm::toString(ZeroLayout.takeError());
  auto Unreadable = Image;
  for (auto &H : Unreadable.ELFMetadata->ProgramHeaders)
    if (H.Type == llvm::ELF::PT_LOAD)
      H.Flags &= ~llvm::ELF::PF_R;
  rejects(Unreadable);
  auto Invalid = Image;
  auto Stack =
      llvm::find_if(Invalid.ELFMetadata->ProgramHeaders, [](const auto &H) {
        return H.Type == llvm::ELF::PT_GNU_STACK;
      });
  ASSERT_NE(Stack, Invalid.ELFMetadata->ProgramHeaders.end());
  *Stack = *Template;
  rejects(Invalid);
#endif
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
