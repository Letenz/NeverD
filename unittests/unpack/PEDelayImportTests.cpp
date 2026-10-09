//===- PEDelayImportTests.cpp - Fresh-process delay import state ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"
#include "unpack/format/pe/PEImage.h"

#include "neverd/emulation/GuestMemory.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <cstring>

namespace neverd::unpack {
namespace {
using namespace llvm::object;
using namespace llvm::support::endian;

template <typename T>
void store(std::vector<uint8_t> &Bytes, uint64_t At, const T &Record) {
  std::memcpy(Bytes.data() + At, &Record, sizeof(Record));
}

class PEDelayImports : public testing::Test {
protected:
  static constexpr uint64_t Base = 0x180000000, Gate = 0x70010000;
  static constexpr uint32_t Directory = 0x2000, Handle = 0x2080;
  static constexpr uint32_t IAT = 0x2100, INT = 0x2140, Module = 0x2180;
  static constexpr uint32_t Name = 0x21a0, Unload = 0x21c0;
  static constexpr uint32_t Thunk = 0x1020;
  pe32plus_header Header{};
  std::vector<uint8_t> File;
  Capture C{};

  void SetUp() override {
    Header.Magic = llvm::COFF::PE32Header::PE32_PLUS;
    Header.ImageBase = Base;
    Header.AddressOfEntryPoint = 0x1000;
    Header.SectionAlignment = 0x1000;
    Header.FileAlignment = 0x200;
    Header.SizeOfHeaders = 0x200;
    Header.SizeOfImage = 0x3000;
    Header.NumberOfRvaAndSize = 16;
    Header.Subsystem = llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI;
    File.resize(0x1600);
    dos_header DOS{};
    DOS.Magic[0] = 'M';
    DOS.Magic[1] = 'Z';
    DOS.AddressOfNewExeHeader = 0x80;
    store(File, 0, DOS);
    std::memcpy(File.data() + 0x80, llvm::COFF::PEMagic, 4);
    coff_file_header COFF{};
    COFF.Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
    COFF.NumberOfSections = 2;
    COFF.SizeOfOptionalHeader = sizeof(Header) + 16 * sizeof(data_directory);
    COFF.Characteristics = llvm::COFF::IMAGE_FILE_EXECUTABLE_IMAGE;
    store(File, 0x84, COFF);
    store(File, 0x98, Header);
    data_directory D{};
    D.RelativeVirtualAddress = Directory;
    D.Size = 2 * sizeof(delay_import_directory_table_entry);
    store(File,
          0x98 + sizeof(Header) +
              llvm::COFF::DELAY_IMPORT_DESCRIPTOR * sizeof(D),
          D);
    coff_section Text{}, Data{};
    std::memcpy(Text.Name, ".text", 5);
    Text.VirtualAddress = 0x1000;
    Text.VirtualSize = 0x400;
    Text.SizeOfRawData = 0x400;
    Text.PointerToRawData = 0x200;
    Text.Characteristics = llvm::COFF::IMAGE_SCN_CNT_CODE |
                           llvm::COFF::IMAGE_SCN_MEM_READ |
                           llvm::COFF::IMAGE_SCN_MEM_EXECUTE;
    std::memcpy(Data.Name, ".data", 5);
    Data.VirtualAddress = 0x2000;
    Data.VirtualSize = 0x1000;
    Data.SizeOfRawData = 0x1000;
    Data.PointerToRawData = 0x600;
    Data.Characteristics = llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                           llvm::COFF::IMAGE_SCN_MEM_READ |
                           llvm::COFF::IMAGE_SCN_MEM_WRITE;
    store(File, 0x98 + COFF.SizeOfOptionalHeader, Text);
    store(File, 0x98 + COFF.SizeOfOptionalHeader + sizeof(Text), Data);
    C.Base = Base;
    C.EntryRVA = 0x1000;
    C.Source = EntrySource::Transfer;
    C.Memory.resize(Header.SizeOfImage);
    C.Memory[0x1000] = C.Memory[Thunk] = C.Memory[Thunk + 8] = 0xc3;
    delay_import_directory_table_entry Desc{};
    Desc.Attributes = 1;
    Desc.Name = Module;
    Desc.ModuleHandle = Handle;
    Desc.DelayImportAddressTable = IAT;
    Desc.DelayImportNameTable = INT;
    store(C.Memory, Directory, Desc);
    write64le(C.Memory.data() + IAT, Base + Thunk);
    write64le(C.Memory.data() + IAT + 8, Base + Thunk + 8);
    write64le(C.Memory.data() + INT, Name);
    write64le(C.Memory.data() + INT + 8, (1ull << 63) | 7);
    std::memcpy(C.Memory.data() + Module, "example.dll", 12);
    std::memcpy(C.Memory.data() + Name + 2, "first", 6);
    C.Memory[0x2f00] = 0x5a;
    C.PageAccess = {emulation::Read, emulation::Read | emulation::Execute,
                    emulation::Read | emulation::Write};
    C.Baseline = C.Memory;
    std::copy_n(C.Memory.begin() + 0x1000, 0x400, File.begin() + 0x200);
    std::copy_n(C.Memory.begin() + 0x2000, 0x1000, File.begin() + 0x600);
    C.Exports.emplace(Gate, ExportBinding{"example.dll", "first", {}});
    C.Exports.emplace(Gate + 8, ExportBinding{"example.dll", {}, 7});
  }

  llvm::Expected<RebuiltImage> rebuild() {
    auto In = pe::Image::read(File);
    if (!In)
      return In.takeError();
    return pe::rebuild(**In, C, {});
  }

  void expectFailure() {
    auto Result = rebuild();
    ASSERT_FALSE(bool(Result));
    const auto Error = llvm::toString(Result.takeError());
    EXPECT_NE(Error.find("delay-import"), std::string::npos) << Error;
  }
};

TEST_F(PEDelayImports, ResolvedCellsReturnToTheirLoaderThunks) {
  for (unsigned Resolved : {1u, 2u}) {
    SCOPED_TRACE(Resolved);
    C.Memory = C.Baseline;
    write64le(C.Memory.data() + Handle, 0x70000000);
    for (unsigned I = 0; I < Resolved; ++I)
      write64le(C.Memory.data() + IAT + 8 * I, Gate + 8 * I);
    auto Result = rebuild();
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    auto Image = test::readImage(Result->File);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(read64le(Image.Mapped.data() + Handle), 0u);
    EXPECT_EQ(read64le(Image.Mapped.data() + IAT), Base + Thunk);
    EXPECT_EQ(read64le(Image.Mapped.data() + IAT + 8), Base + Thunk + 8);
    EXPECT_EQ(Image.Mapped[0x2f00], 0x5a);
    EXPECT_TRUE(Result->Imports.empty());
    EXPECT_EQ(Image.directory(llvm::COFF::DELAY_IMPORT_DESCRIPTOR).Size,
              2 * sizeof(delay_import_directory_table_entry));
  }
}

TEST_F(PEDelayImports, PendingCellsAndMetadataRemainByteExact) {
  auto Result = rebuild();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  auto Image = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(std::equal(C.Memory.begin() + 0x2000, C.Memory.end(),
                         Image.Mapped.begin() + 0x2000));
  EXPECT_TRUE(Result->Imports.empty());
}

TEST_F(PEDelayImports, CompleteUnloadTablesAuthorizeMaterializedThunks) {
  write32le(C.Memory.data() + Directory + 24, Unload);
  write64le(C.Memory.data() + Unload, Base + Thunk);
  write64le(C.Memory.data() + Unload + 8, Base + Thunk + 8);
  // The packed file did not initially contain these thunks in its IAT.
  write64le(C.Baseline.data() + IAT, 0);
  write64le(C.Memory.data() + IAT, Gate);
  write64le(C.Memory.data() + Handle, 0x70000000);
  auto Result = rebuild();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  auto Image = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(read64le(Image.Mapped.data() + Handle), 0u);
  EXPECT_EQ(read64le(Image.Mapped.data() + IAT), Base + Thunk);
  EXPECT_EQ(read64le(Image.Mapped.data() + IAT + 8), Base + Thunk + 8);
  EXPECT_EQ(read64le(Image.Mapped.data() + Unload), Base + Thunk);
}

TEST_F(PEDelayImports, BoundCachesCannotBecomeOrdinaryImports) {
  write32le(C.Memory.data() + Directory + 20, Unload);
  write32le(C.Memory.data() + Directory + 28, 37);
  C.Baseline = C.Memory;
  write64le(C.Memory.data() + Unload, Gate);
  write64le(C.Memory.data() + Unload + 8, Gate + 8);
  write64le(C.Memory.data() + IAT, Gate);
  auto Result = rebuild();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  auto Image = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(Result->Imports.empty());
  EXPECT_EQ(read32le(Image.Mapped.data() + Directory + 20), 0u);
  EXPECT_EQ(read32le(Image.Mapped.data() + Directory + 28), 0u);
  EXPECT_EQ(read64le(Image.Mapped.data() + Unload), Gate);
}

TEST_F(PEDelayImports, InitialThunksUseTheObservedImageBase) {
  C.Base += 0x100000000;
  for (unsigned I = 0; I < 2; ++I) {
    write64le(C.Baseline.data() + IAT + 8 * I, C.Base + Thunk + 8 * I);
    write64le(C.Memory.data() + IAT + 8 * I, Gate + 8 * I);
  }
  auto Result = rebuild();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  auto Image = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Image.Base, C.Base);
  EXPECT_EQ(read64le(Image.Mapped.data() + IAT), C.Base + Thunk);
}

TEST_F(PEDelayImports, ZeroTerminatorsKeepTheirCompleteFileBacking) {
  const uint64_t NewIAT = 0x21f8;
  write32le(C.Memory.data() + Directory + 12, NewIAT);
  write64le(C.Memory.data() + NewIAT, Base + Thunk);
  write64le(C.Memory.data() + INT + 8, 0);
  C.Memory[0x2f00] = 0;
  C.Baseline = C.Memory;
  write64le(C.Memory.data() + NewIAT, Gate);
  auto Result = rebuild();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  auto Image = test::readImage(Result->File);
  ASSERT_FALSE(HasFailure());
  EXPECT_GE(Image.Sections[1].FileSize, NewIAT - 0x2000 + 16);
}

TEST_F(PEDelayImports, MissingThunkEvidenceFailsInsteadOfKeepingGuestPointers) {
  for (uint64_t Initial : {uint64_t(0), Gate, Base + Handle, Base + 0x4000}) {
    SCOPED_TRACE(Initial);
    write64le(C.Baseline.data() + IAT, Initial);
    write64le(C.Memory.data() + IAT, Gate);
    expectFailure();
  }
}

TEST_F(PEDelayImports, UnknownResolvedTargetsCannotAuthorizeRestoration) {
  write64le(C.Memory.data() + IAT, Gate + 0x1000);
  expectFailure();
}

TEST_F(PEDelayImports, ChangedDescriptorsNeedACompleteUnloadTable) {
  write32le(C.Memory.data() + Directory + 28, 99);
  expectFailure();
}

TEST_F(PEDelayImports, IncompleteTablesAndNamesFailExplicitly) {
  const auto Good = C.Memory;
  for (auto [Field, Value] : {std::pair{0u, 0u},
                              {0u, 3u},
                              {4u, 0u},
                              {8u, 0x2ffcu},
                              {12u, 0x2ffcu},
                              {16u, 0x2ffcu},
                              {20u, 0x2ffcu},
                              {24u, 0x2ffcu}}) {
    SCOPED_TRACE(Field);
    C.Memory = Good;
    write32le(C.Memory.data() + Directory + Field, Value);
    C.Baseline = C.Memory;
    expectFailure();
  }
  C.Memory = Good;
  C.Memory[Module] = 0;
  C.Baseline = C.Memory;
  expectFailure();
  C.Memory = Good;
  write64le(C.Memory.data() + INT, (1ull << 63) | 0x10000);
  C.Baseline = C.Memory;
  expectFailure();
}

TEST_F(PEDelayImports, BothTheDescriptorAndThunkArraysNeedTerminators) {
  const uint64_t D =
      0x98 + sizeof(Header) +
      llvm::COFF::DELAY_IMPORT_DESCRIPTOR * sizeof(data_directory);
  write32le(File.data() + D + 4, sizeof(delay_import_directory_table_entry));
  expectFailure();
  write32le(File.data() + D + 4,
            2 * sizeof(delay_import_directory_table_entry));
  write64le(C.Memory.data() + IAT + 16, Base + Thunk);
  expectFailure();
}

TEST_F(PEDelayImports, OverlappingOwnersCannotResetEachOthersStorage) {
  for (uint32_t Address : {INT, Directory, IAT + 16, IAT + 8}) {
    SCOPED_TRACE(Address);
    const auto Good = C.Memory;
    write32le(C.Memory.data() + Directory + 8, Address);
    C.Baseline = C.Memory;
    // A handle from a prior invocation cannot establish initial state.
    expectFailure();
    C.Memory = Good;
  }
  // Another retained directory owns the handle even when its bytes are zero.
  const uint64_t D =
      0x98 + sizeof(Header) + llvm::COFF::TLS_TABLE * sizeof(data_directory);
  write32le(File.data() + D, Handle);
  write32le(File.data() + D + 4, sizeof(coff_tls_directory64));
  C.Baseline = C.Memory;
  expectFailure();
}

TEST_F(PEDelayImports, UnloadTablesCannotAliasTheLiveIAT) {
  write32le(C.Memory.data() + Directory + 24, IAT);
  expectFailure();
}
} // namespace
} // namespace neverd::unpack
