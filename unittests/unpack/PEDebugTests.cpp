//===- PEDebugTests.cpp - Debug directory file placement ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"
#include "unpack/format/pe/PEImage.h"

#include "neverd/emulation/GuestMemory.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace neverd::unpack {
namespace {
using namespace llvm::object;

template <typename T>
void store(std::vector<uint8_t> &Bytes, uint64_t Offset, const T &Record) {
  std::memcpy(Bytes.data() + Offset, &Record, sizeof(Record));
}

/// An independent two-section PE with movable debug records and payloads.
struct DebugImage {
  pe32plus_header Header{};
  std::array<coff_section, 2> Sections{};
  std::array<data_directory, 16> Directories{};
  std::vector<uint8_t> Text = std::vector<uint8_t>(0x400);
  std::vector<uint8_t> Data = std::vector<uint8_t>(0x2000);

  DebugImage() {
    Header.Magic = llvm::COFF::PE32Header::PE32_PLUS;
    Header.ImageBase = 0x180000000;
    Header.AddressOfEntryPoint = 0x1000;
    Header.SectionAlignment = 0x1000;
    Header.FileAlignment = 0x200;
    Header.SizeOfHeaders = 0x200;
    Header.SizeOfImage = 0x4000;
    Header.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
    Header.NumberOfRvaAndSize = Directories.size();
    auto &Code = Sections[0];
    std::memcpy(Code.Name, ".text", 5);
    Code.VirtualAddress = 0x1000;
    Code.VirtualSize = Text.size();
    Code.SizeOfRawData = Text.size();
    Code.PointerToRawData = 0x200;
    Code.Characteristics = llvm::COFF::IMAGE_SCN_CNT_CODE |
                           llvm::COFF::IMAGE_SCN_MEM_READ |
                           llvm::COFF::IMAGE_SCN_MEM_EXECUTE;
    Text[0] = 0xc3;
    auto &Storage = Sections[1];
    std::memcpy(Storage.Name, ".data", 5);
    Storage.VirtualAddress = 0x2000;
    Storage.VirtualSize = Data.size();
    Storage.SizeOfRawData = Data.size();
    Storage.PointerToRawData = 0x600;
    Storage.Characteristics = llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                              llvm::COFF::IMAGE_SCN_MEM_READ |
                              llvm::COFF::IMAGE_SCN_MEM_WRITE;
    Data[0] = 0x42;
    Data[0x1ff0] = 0x7b;
  }

  std::vector<uint8_t> file() const {
    std::vector<uint8_t> Bytes(Sections[1].PointerToRawData + Data.size());
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
    COFF.Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE;
    store(Bytes, 0x84, COFF);
    store(Bytes, 0x98, Header);
    store(Bytes, 0x98 + sizeof(Header), Directories);
    store(Bytes, 0x98 + COFF.SizeOfOptionalHeader, Sections);
    std::copy(Text.begin(), Text.end(),
              Bytes.begin() + Sections[0].PointerToRawData);
    std::copy(Data.begin(), Data.end(),
              Bytes.begin() + Sections[1].PointerToRawData);
    return Bytes;
  }

  Capture capture() const {
    Capture C{};
    C.Base = Header.ImageBase;
    C.EntryRVA = Header.AddressOfEntryPoint;
    C.Source = EntrySource::Transfer;
    C.Memory.resize(Header.SizeOfImage);
    C.PageAccess.resize(C.Memory.size() / 4096);
    std::copy(Text.begin(), Text.end(),
              C.Memory.begin() + Sections[0].VirtualAddress);
    std::copy(Data.begin(), Data.end(),
              C.Memory.begin() + Sections[1].VirtualAddress);
    C.PageAccess[Sections[0].VirtualAddress / 4096] =
        emulation::Read | emulation::Execute;
    for (uint64_t Offset = 0; Offset < Data.size(); Offset += 4096)
      C.PageAccess[(Sections[1].VirtualAddress + Offset) / 4096] =
          emulation::Read | emulation::Write;
    C.Baseline = C.Memory;
    return C;
  }
};

TEST(PEDebugRecords, DebugFileOffsetSurvivesATrimmedAlignmentBoundary) {
  DebugImage Fixture;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
      0x11e8;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size =
      sizeof(debug_directory);
  debug_directory Debug{};
  Debug.Type = llvm::COFF::IMAGE_DEBUG_TYPE_CODEVIEW;
  Debug.SizeOfData = 1;
  Debug.AddressOfRawData = Fixture.Sections[1].VirtualAddress;
  // PointerToRawData is the zero field at 0x200. Rebuilding changes it only
  // after the preceding nonzero field has ended on a file-alignment boundary.
  store(Fixture.Text, 0x1e8, Debug);
  const auto Bytes = Fixture.file();
  auto Input = pe::Image::read(Bytes);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  auto Result = pe::rebuild(**Input, Fixture.capture(), {});
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto Rebuilt = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  const uint64_t Field = 0x11e8 + offsetof(debug_directory, PointerToRawData);
  const uint64_t Payload = Rebuilt.Sections[1].FileOffset;
  EXPECT_GE(Rebuilt.Sections[0].FileSize, 0x204u);
  EXPECT_EQ(llvm::support::endian::read32le(Rebuilt.Mapped.data() + Field),
            Payload);
  ASSERT_LT(Payload, Result->File.size());
  EXPECT_EQ(Result->File[Payload], 0x42u);
}

TEST(PEDebugRecords, AllZeroDebugPayloadRetainsItsCompleteFileRange) {
  DebugImage Fixture;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
      0x11e8;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size =
      sizeof(debug_directory);
  debug_directory Debug{};
  Debug.SizeOfData = 0x400;
  Debug.AddressOfRawData = 0x3800;
  store(Fixture.Text, 0x1e8, Debug);
  std::fill(Fixture.Data.begin(), Fixture.Data.end(), 0);
  const auto Bytes = Fixture.file();
  auto Input = pe::Image::read(Bytes);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  auto Result = pe::rebuild(**Input, Fixture.capture(), {});
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto Rebuilt = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  const uint64_t Field = 0x11e8 + offsetof(debug_directory, PointerToRawData);
  const uint64_t Payload = Rebuilt.Sections[1].FileOffset + 0x1800;
  EXPECT_GE(Rebuilt.Sections[1].FileSize, 0x1c00u);
  EXPECT_EQ(llvm::support::endian::read32le(Rebuilt.Mapped.data() + Field),
            Payload);
  ASSERT_LE(Payload + Debug.SizeOfData, Result->File.size());
  EXPECT_TRUE(std::all_of(Result->File.begin() + Payload,
                          Result->File.begin() + Payload + Debug.SizeOfData,
                          [](uint8_t Byte) { return Byte == 0; }));
}

TEST(PEDebugRecords, DebugRecordsRetainTheirFileStorageWithoutAPayload) {
  DebugImage Fixture;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
      0x11e8;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size =
      sizeof(debug_directory);
  const auto Bytes = Fixture.file();
  auto Input = pe::Image::read(Bytes);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  auto Result = pe::rebuild(**Input, Fixture.capture(), {});
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto Rebuilt = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  EXPECT_GE(Rebuilt.Sections[0].FileSize, 0x204u);
  EXPECT_EQ(Rebuilt.directory(llvm::COFF::DEBUG_DIRECTORY).Size,
            sizeof(debug_directory));
}

TEST(PEDebugRecords, FileOnlyDebugPayloadKeepsItsRelativeOverlayPosition) {
  for (uint32_t Prefix : {0u, 17u}) {
    SCOPED_TRACE(Prefix);
    DebugImage Fixture;
    Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
        0x1100;
    Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size =
        sizeof(debug_directory);
    const uint64_t InputEnd =
        Fixture.Sections[1].PointerToRawData + Fixture.Data.size();
    debug_directory Debug{};
    Debug.Type = llvm::COFF::IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS;
    Debug.SizeOfData = 4;
    Debug.PointerToRawData = InputEnd + Prefix;
    store(Fixture.Text, 0x100, Debug);
    auto Bytes = Fixture.file();
    std::vector<uint8_t> Overlay(Prefix, 0xa5);
    // Zero extended DLL characteristics are valid. A stale zero file pointer
    // would instead read the nonzero DOS signature as these flags.
    Overlay.insert(Overlay.end(), Debug.SizeOfData, 0);
    Overlay.push_back(0x7b);
    Bytes.insert(Bytes.end(), Overlay.begin(), Overlay.end());
    auto Input = pe::Image::read(Bytes);
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    auto Result = pe::rebuild(**Input, Fixture.capture(), {});
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const auto Rebuilt = test::readImage(Result->File);
    ASSERT_FALSE(HasFailure());
    const uint64_t OutputEnd =
        Rebuilt.Sections.back().FileOffset + Rebuilt.Sections.back().FileSize;
    ASSERT_NE(OutputEnd, InputEnd);
    const uint64_t Field = 0x1100 + offsetof(debug_directory, PointerToRawData);
    const uint64_t Payload =
        llvm::support::endian::read32le(Rebuilt.Mapped.data() + Field);
    EXPECT_EQ(Payload, OutputEnd + Prefix);
    ASSERT_LE(Payload + Debug.SizeOfData, Result->File.size());
    EXPECT_EQ(llvm::support::endian::read32le(Result->File.data() + Payload),
              0u);
    EXPECT_EQ(llvm::support::endian::read32le(
                  Rebuilt.Mapped.data() + 0x1100 +
                  offsetof(debug_directory, AddressOfRawData)),
              0u);
    ASSERT_EQ(Result->File.size(), OutputEnd + Overlay.size());
    EXPECT_TRUE(std::equal(Overlay.begin(), Overlay.end(),
                           Result->File.begin() + OutputEnd));
  }
}

TEST(PEDebugRecords, FileOnlyDebugPayloadMustFitThePreservedOverlay) {
  for (uint32_t Offset : {0u, 0x25ffu, 0x2605u, 0x2609u, UINT32_MAX}) {
    SCOPED_TRACE(Offset);
    DebugImage Fixture;
    Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
        0x1100;
    Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size =
        sizeof(debug_directory);
    debug_directory Debug{};
    Debug.SizeOfData = 4;
    Debug.PointerToRawData = Offset;
    store(Fixture.Text, 0x100, Debug);
    auto Bytes = Fixture.file();
    ASSERT_EQ(Bytes.size(), 0x2600u);
    Bytes.resize(Bytes.size() + 8, 0);
    auto Input = pe::Image::read(Bytes);
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    auto Result = pe::rebuild(**Input, Fixture.capture(), {});
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const auto Rebuilt = test::readImage(Result->File);
    ASSERT_FALSE(HasFailure());
    const uint64_t Field = 0x1100 + offsetof(debug_directory, PointerToRawData);
    EXPECT_EQ(llvm::support::endian::read32le(Rebuilt.Mapped.data() + Field),
              0u);
  }
}

TEST(PEDebugRecords, UnmappedDebugPayloadCannotAcquireAFileOffset) {
  DebugImage Fixture;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
      0x11e8;
  Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size =
      sizeof(debug_directory);
  debug_directory Debug{};
  Debug.SizeOfData = 8;
  Debug.AddressOfRawData = 0x3ffc;
  Debug.PointerToRawData =
      Fixture.Sections[1].PointerToRawData + Fixture.Data.size();
  store(Fixture.Text, 0x1e8, Debug);
  auto Bytes = Fixture.file();
  Bytes.resize(Bytes.size() + Debug.SizeOfData, 0);
  auto Input = pe::Image::read(Bytes);
  ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
  auto Result = pe::rebuild(**Input, Fixture.capture(), {});
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  const auto Rebuilt = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  const uint64_t Field = 0x11e8 + offsetof(debug_directory, PointerToRawData);
  EXPECT_EQ(llvm::support::endian::read32le(Rebuilt.Mapped.data() + Field), 0u);
}

TEST(PEDebugRecords, MalformedDebugRecordsAreRemoved) {
  for (const auto [RVA, Size] :
       {std::pair{0x3ff0u, uint32_t(sizeof(debug_directory))},
        std::pair{0x1100u, uint32_t(sizeof(debug_directory) - 1)},
        std::pair{0x800u, uint32_t(sizeof(debug_directory))}}) {
    SCOPED_TRACE(RVA);
    DebugImage Fixture;
    Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].RelativeVirtualAddress =
        RVA;
    Fixture.Directories[llvm::COFF::DEBUG_DIRECTORY].Size = Size;
    const auto Bytes = Fixture.file();
    auto Input = pe::Image::read(Bytes);
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    auto Result = pe::rebuild(**Input, Fixture.capture(), {});
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const auto Rebuilt = test::readImage(Result->File);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Rebuilt.directory(llvm::COFF::DEBUG_DIRECTORY).Size, 0u);
    EXPECT_EQ(
        Rebuilt.directory(llvm::COFF::DEBUG_DIRECTORY).RelativeVirtualAddress,
        0u);
  }
}
} // namespace
} // namespace neverd::unpack
