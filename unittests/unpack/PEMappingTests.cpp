//===- PEMappingTests.cpp - Native Windows user-image page ranges --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"
#include "unpack/format/pe/PEImage.h"

#include "neverd/emulation/GuestMemory.h"
#include "neverd/object/PELayout.h"

#include "llvm/Support/Endian.h"

#ifdef NEVERD_UNPACK_TEST_EXECUTION
#include "emulation/os/windows/process/WindowsProcess.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace neverd::unpack {
namespace {
using namespace llvm::object;

struct MappingCase {
  const char *Name;
  uint32_t SectionAlignment, FileAlignment, VirtualSize, RawSize, MappedSize;
};

// Independent observations from native x64 LoadLibraryExA, VirtualQuery and
// ReadProcessMemory on Windows Server 2022 (10.0.20348). Normal DLL loading
// and DONT_RESOLVE_DLL_REFERENCES agreed on these bytes and page ranges.
constexpr MappingCase Cases[] = {
    {"page-full", 4096, 512, 8192, 8192, 8192},
    {"page-raw-tail", 4096, 512, 0x100, 8192, 4096},
    {"page-one-page", 4096, 512, 4096, 4096, 4096},
    {"page-zero-virtual", 4096, 512, 0, 8192, 8192},
    {"large-full", 65536, 512, 8192, 8192, 8192},
    {"large-raw-tail", 65536, 512, 0x100, 8192, 4096},
    {"large-one-page", 65536, 512, 4096, 4096, 4096},
    {"large-bss", 65536, 65536, 4096, 0, 4096},
    {"large-raw-padding", 65536, 65536, 0x100, 65536, 4096},
};

template <typename T>
void store(std::vector<uint8_t> &Bytes, uint64_t Offset, const T &Record) {
  std::memcpy(Bytes.data() + Offset, &Record, sizeof(Record));
}

struct MappingImage {
  const MappingCase &Case;
  pe32plus_header Header{};
  std::array<coff_section, 2> Sections{};
  std::array<data_directory, 16> Directories{};
  std::vector<uint8_t> Text, Data;

  explicit MappingImage(const MappingCase &C)
      : Case(C), Text(C.FileAlignment), Data(C.RawSize) {
    Header.Magic = llvm::COFF::PE32Header::PE32_PLUS;
    Header.ImageBase = 0x180000000;
    Header.AddressOfEntryPoint = C.SectionAlignment;
    Header.SectionAlignment = C.SectionAlignment;
    Header.FileAlignment = C.FileAlignment;
    Header.SizeOfHeaders = C.FileAlignment;
    Header.SizeOfImage =
        2 * C.SectionAlignment + (C.SectionAlignment == 4096 ? 8192 : 65536);
    Header.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
    Header.NumberOfRvaAndSize = Directories.size();
    auto &Code = Sections[0];
    std::memcpy(Code.Name, ".text", 5);
    Code.VirtualAddress = C.SectionAlignment;
    Code.VirtualSize = 6;
    Code.SizeOfRawData = Text.size();
    Code.PointerToRawData = Header.SizeOfHeaders;
    Code.Characteristics = llvm::COFF::IMAGE_SCN_CNT_CODE |
                           llvm::COFF::IMAGE_SCN_MEM_READ |
                           llvm::COFF::IMAGE_SCN_MEM_EXECUTE;
    Text[0] = 0xb8;
    Text[1] = 1;
    Text[5] = 0xc3;
    auto &Probe = Sections[1];
    std::memcpy(Probe.Name, ".probe", 6);
    Probe.VirtualAddress = 2 * C.SectionAlignment;
    Probe.VirtualSize = C.VirtualSize;
    Probe.SizeOfRawData = C.RawSize;
    Probe.PointerToRawData = C.RawSize ? 2 * C.FileAlignment : 0;
    Probe.Characteristics =
        llvm::COFF::IMAGE_SCN_MEM_READ | llvm::COFF::IMAGE_SCN_MEM_WRITE |
        (C.RawSize ? llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
                   : llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA);
    for (const auto [Offset, Byte] :
         {std::pair{0x100u, 0x4au}, std::pair{0xfffu, 0x5cu},
          std::pair{0x1000u, 0x7bu}, std::pair{0x1fffu, 0x6du}})
      if (Offset < Data.size())
        Data[Offset] = Byte;
  }

  std::vector<uint8_t> file() const {
    std::vector<uint8_t> Bytes(2 * Case.FileAlignment + Data.size());
    dos_header DOS{};
    DOS.Magic[0] = 'M';
    DOS.Magic[1] = 'Z';
    DOS.AddressOfNewExeHeader = 0x80;
    store(Bytes, 0, DOS);
    std::memcpy(Bytes.data() + 0x80, llvm::COFF::PEMagic, 4);
    coff_file_header COFF{};
    COFF.Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
    COFF.NumberOfSections = Sections.size();
    COFF.SizeOfOptionalHeader = sizeof(Header) + sizeof(Directories);
    COFF.Characteristics =
        llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE | llvm::COFF::IMAGE_FILE_DLL;
    store(Bytes, 0x84, COFF);
    store(Bytes, 0x98, Header);
    store(Bytes, 0x98 + sizeof(Header), Directories);
    store(Bytes, 0x98 + COFF.SizeOfOptionalHeader, Sections);
    std::copy(Text.begin(), Text.end(),
              Bytes.begin() + Sections[0].PointerToRawData);
    std::copy(Data.begin(), Data.end(), Bytes.begin() + 2 * Case.FileAlignment);
    return Bytes;
  }

  Capture capture() const {
    Capture C{};
    C.Base = Header.ImageBase;
    C.EntryRVA = Header.AddressOfEntryPoint;
    C.Source = EntrySource::Transfer;
    C.Memory.resize(Header.SizeOfImage);
    C.PageAccess.resize(C.Memory.size() / 4096);
    std::copy_n(Text.begin(), std::min<size_t>(Text.size(), 4096),
                C.Memory.begin() + Sections[0].VirtualAddress);
    std::copy_n(Data.begin(), std::min<size_t>(Data.size(), Case.MappedSize),
                C.Memory.begin() + Sections[1].VirtualAddress);
    C.PageAccess[Sections[0].VirtualAddress / 4096] =
        emulation::Read | emulation::Execute;
    for (uint64_t Offset = 0; Offset < Case.MappedSize; Offset += 4096)
      C.PageAccess[(Sections[1].VirtualAddress + Offset) / 4096] =
          emulation::Read | emulation::Write;
    C.Baseline = C.Memory;
    return C;
  }
};

class PEMapping : public testing::Test {
#ifdef NEVERD_UNPACK_TEST_EXECUTION
protected:
  std::filesystem::path Directory;

  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-pe-mapping", Path));
    Directory = Path.str().str();
  }
  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
  }
  llvm::Expected<emulation::windows_process::Image>
  load(const MappingCase &Case, llvm::ArrayRef<uint8_t> Bytes) {
    const auto Path = Directory / (std::string(Case.Name) + ".dll");
    std::ofstream Stream(Path, std::ios::binary);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.close();
    if (!Stream)
      return llvm::createStringError("could not write PE mapping fixture");
    emulation::windows_process::ImageReadBudget Budget{1 << 20, 1 << 20};
    return emulation::windows_process::loadProgramImage(Path, Budget, true,
                                                        true);
  }
#endif
};

TEST_F(PEMapping, NativeSectionRangesAgreeWithUnpackAndRuntime) {
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    MappingImage Fixture(Case);
    const auto Bytes = Fixture.file();
    auto Input = pe::Image::read(Bytes);
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    ASSERT_EQ((*Input)->regions().size(), 2u);
    const auto &Region = (*Input)->regions()[1];
    EXPECT_EQ(Region.MemorySize, Case.MappedSize);
    EXPECT_EQ(Region.FileSize, Case.RawSize);
    EXPECT_EQ((*Input)->regions()[0].MemorySize, 4096u);
    EXPECT_NE((*Input)->regionAt(Region.RVA + Case.MappedSize - 1), nullptr);
    EXPECT_EQ((*Input)->regionAt(Region.RVA + Case.MappedSize), nullptr);
#ifdef NEVERD_UNPACK_TEST_EXECUTION
    auto Runtime = load(Case, Bytes);
    ASSERT_TRUE(bool(Runtime)) << llvm::toString(Runtime.takeError());
    ASSERT_EQ(Runtime->Regions.size(), 3u);
    EXPECT_EQ(Runtime->Regions[1].Bytes.size(), 4096u);
    const auto &Mapped = Runtime->Regions[2];
    EXPECT_EQ(Mapped.ContentSize,
              Case.VirtualSize ? Case.VirtualSize : Case.RawSize);
    ASSERT_EQ(Mapped.Bytes.size(), Case.MappedSize);
    for (uint64_t Offset = 0; Offset < Case.MappedSize; ++Offset)
      ASSERT_EQ(Mapped.Bytes[Offset],
                Offset < Fixture.Data.size() ? Fixture.Data[Offset] : 0)
          << "offset=" << Offset;
#endif
  }
}

TEST_F(PEMapping, RebuildingPreservesNativeRangesAndFileAlignment) {
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    MappingImage Fixture(Case);
    const auto Bytes = Fixture.file();
    auto Input = pe::Image::read(Bytes);
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    const auto Capture = Fixture.capture();
    auto Result = pe::rebuild(**Input, Capture, {});
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    auto Reparsed = pe::Image::read(Result->File);
    ASSERT_TRUE(bool(Reparsed)) << llvm::toString(Reparsed.takeError());
    EXPECT_EQ((*Reparsed)->headers().FileAlignment, Case.FileAlignment);
    ASSERT_EQ((*Reparsed)->regions().size(), 2u);
    EXPECT_EQ((*Reparsed)->regions()[1].MemorySize, Case.MappedSize);
    const auto Rebuilt = test::readImage(Result->File);
    ASSERT_FALSE(HasFailure());
    const auto &Probe = Rebuilt.Sections[1];
    EXPECT_TRUE(std::equal(Capture.Memory.begin() + Probe.RVA,
                           Capture.Memory.begin() + Probe.RVA + Case.MappedSize,
                           Rebuilt.Mapped.begin() + Probe.RVA));
    // Raw file alignment may be larger than the accessible section range.
    if (Probe.FileSize > Case.MappedSize)
      EXPECT_TRUE(
          std::all_of(Result->File.begin() + Probe.FileOffset + Case.MappedSize,
                      Result->File.begin() + Probe.FileOffset + Probe.FileSize,
                      [](uint8_t Byte) { return Byte == 0; }));
  }
}

TEST_F(PEMapping, RawFileValidationIncludesUnmappedTailBytes) {
  const auto &Case = Cases[1];
  MappingImage Fixture(Case);
  auto Bytes = Fixture.file();
  Bytes.resize(Fixture.Sections[1].PointerToRawData + Case.MappedSize);
  auto Input = pe::Image::read(Bytes);
  ASSERT_FALSE(bool(Input));
  llvm::consumeError(Input.takeError());
#ifdef NEVERD_UNPACK_TEST_EXECUTION
  auto Runtime = load(Case, Bytes);
  ASSERT_FALSE(bool(Runtime));
  llvm::consumeError(Runtime.takeError());
#endif
}

TEST_F(PEMapping, FilePaddingCannotCopyReservedImageBytes) {
  const auto &Case = Cases[7];
  MappingImage Fixture(Case);
  const auto Bytes = Fixture.file();
  auto Input = pe::Image::read(Bytes);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  auto Capture = Fixture.capture();
  const uint64_t RVA = Fixture.Sections[1].VirtualAddress;
  Capture.Memory[RVA] = 0x42;
  std::fill(Capture.Memory.begin() + RVA + Case.MappedSize,
            Capture.Memory.end(), 0x99);
  auto Result = pe::rebuild(**Input, Capture, {});
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto Rebuilt = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  const auto &Probe = Rebuilt.Sections[1];
  ASSERT_EQ(Probe.FileSize, Case.FileAlignment);
  EXPECT_EQ(Probe.VirtualSize, Case.VirtualSize);
  EXPECT_EQ(Result->File[Probe.FileOffset], 0x42u);
  EXPECT_TRUE(
      std::all_of(Result->File.begin() + Probe.FileOffset + Case.MappedSize,
                  Result->File.begin() + Probe.FileOffset + Probe.FileSize,
                  [](uint8_t Byte) { return Byte == 0; }));
}

TEST_F(PEMapping, AllZeroRawPagesRemainAccessibleThroughVirtualSize) {
  const auto &Case = Cases[0];
  MappingImage Fixture(Case);
  std::fill(Fixture.Data.begin() + 4096, Fixture.Data.end(), 0);
  const auto Bytes = Fixture.file();
  auto Input = pe::Image::read(Bytes);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  auto Result = pe::rebuild(**Input, Fixture.capture(), {});
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  auto Reparsed = pe::Image::read(Result->File);
  ASSERT_TRUE(bool(Reparsed)) << llvm::toString(Reparsed.takeError());
  ASSERT_EQ((*Reparsed)->regions()[1].MemorySize, 8192u);
  const auto Rebuilt = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  const uint64_t RVA = Fixture.Sections[1].VirtualAddress;
  EXPECT_TRUE(std::all_of(Rebuilt.Mapped.begin() + RVA + 4096,
                          Rebuilt.Mapped.begin() + RVA + 8192,
                          [](uint8_t Byte) { return Byte == 0; }));
#ifdef NEVERD_UNPACK_TEST_EXECUTION
  auto Runtime = load(Case, Result->File);
  ASSERT_TRUE(bool(Runtime)) << llvm::toString(Runtime.takeError());
  ASSERT_EQ(Runtime->Regions[2].Bytes.size(), 8192u);
  EXPECT_TRUE(std::all_of(Runtime->Regions[2].Bytes.begin() + 4096,
                          Runtime->Regions[2].Bytes.end(),
                          [](uint8_t Byte) { return Byte == 0; }));
#endif
}

TEST_F(PEMapping, PageRoundingCannotWrapThePE32Extent) {
  EXPECT_EQ(getPEUserSectionMappedSize(UINT32_MAX, 0, 4096), uint64_t{1} << 32);
  EXPECT_EQ(getPEUserSectionMappedSize(0, UINT32_MAX, 4096), uint64_t{1} << 32);
  EXPECT_EQ(getPEUserSectionMappedSize(0, 0, 4096), 0u);
  MappingImage Fixture(Cases[0]);
  Fixture.Sections[1].VirtualSize = UINT32_MAX;
  const auto Bytes = Fixture.file();
  auto Input = pe::Image::read(Bytes);
  ASSERT_FALSE(bool(Input));
  llvm::consumeError(Input.takeError());
#ifdef NEVERD_UNPACK_TEST_EXECUTION
  auto Runtime = load(Cases[0], Bytes);
  ASSERT_FALSE(bool(Runtime));
  llvm::consumeError(Runtime.takeError());
#endif
}
} // namespace
} // namespace neverd::unpack
