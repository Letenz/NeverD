//===- PERebuildTests.cpp - Import and TLS reconstruction boundaries ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnpackTestSupport.h"
#include "unpack/format/pe/PEImage.h"

#include "neverd/emulation/GuestMemory.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace neverd::unpack {
namespace {
using namespace llvm::support::endian;
using llvm::object::coff_tls_directory64;

class PERebuild : public testing::Test {
protected:
  void SetUp() override {
    Linked = test::readImage(test::fixture(test::Plain));
    ASSERT_FALSE(HasFailure());
    Bytes = Linked.File;
    prepare();
    ASSERT_FALSE(HasFailure());
    // These are independently chosen export identities, not addresses from
    // a protected sample or from the process model's provider layout.
    C.Exports.emplace(FirstGate, FirstBinding);
    C.Exports.emplace(SecondGate, SecondBinding);
  }

  void prepare() {
    auto Parsed = pe::Image::read(Bytes);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    Input = std::move(*Parsed);
    C = {};
    C.Base = Linked.Base;
    C.EntryRVA = Linked.Entry;
    C.Source = EntrySource::Transfer;
    C.Memory = Linked.Mapped;
    C.Baseline = C.Memory;
    C.PageAccess.assign(Input->extent() / 4096, emulation::Read |
                                                    emulation::Write |
                                                    emulation::Execute);
  }

  void plantCall(uint64_t RVA) {
    uint8_t *Site = C.Memory.data() + RVA;
    Site[0] = 0x50;
    Site[1] = 0xe8;
    write32le(Site + 2, 0x20);
  }

  RebuiltImage rebuild(llvm::ArrayRef<TailImport> Calls) {
    RebuildPlan Plan;
    Plan.TailImports.assign(Calls.begin(), Calls.end());
    auto Result = pe::rebuild(*Input, C, Plan);
    if (!Result) {
      ADD_FAILURE() << llvm::toString(Result.takeError());
      return {};
    }
    return std::move(*Result);
  }

  void prepareTLS() {
    // Place complete independent records in mapped section padding. The
    // input directory names a loader callback; a second record names the
    // generated callback that execution actually reached.
    const uint64_t Region = Input->regions().back().RVA;
    OriginalTLS = Region + 0x100;
    ProgramTLS = Region + 0x180;
    Callbacks = Region + 0x200;
    Callback = Linked.Entry + 0x80;
    const uint64_t Optional =
        Linked.EntryOffset -
        offsetof(llvm::object::pe32plus_header, AddressOfEntryPoint);
    const uint64_t Directory =
        Optional + sizeof(llvm::object::pe32plus_header) +
        llvm::COFF::TLS_TABLE * sizeof(llvm::object::data_directory);
    write32le(Bytes.data() + Directory, OriginalTLS);
    write32le(Bytes.data() + Directory + 4, sizeof(coff_tls_directory64));
    prepare();
    ASSERT_FALSE(HasFailure());
    coff_tls_directory64 Record{};
    Record.AddressOfIndex = C.Base + Region + 0x80;
    Record.StartAddressOfRawData = C.Base + Region + 0x90;
    Record.EndAddressOfRawData = C.Base + Region + 0x98;
    Record.AddressOfCallBacks = C.Base + Region + 0x40;
    std::memcpy(C.Memory.data() + OriginalTLS, &Record, sizeof(Record));
    write64le(C.Memory.data() + Region + 0x40, C.Base + Linked.Entry);
    write64le(C.Memory.data() + Region + 0x48, 0);
    C.Baseline = C.Memory;
    Record.AddressOfCallBacks = C.Base + Callbacks;
    std::memcpy(C.Memory.data() + ProgramTLS, &Record, sizeof(Record));
    write64le(C.Memory.data() + Callbacks, C.Base + Callback);
    write64le(C.Memory.data() + Callbacks + 8, 0);
    C.Transfers = {{Callback, false, 1, true}, {Linked.Entry, true, 1, true}};
    C.ThreadLocal.emplace(C.Memory.begin() + Region + 0x90,
                          C.Memory.begin() + Region + 0x98);
  }

  void prepareTLSWithoutCallbacks(bool EmptyArray) {
    prepareTLS();
    ASSERT_FALSE(HasFailure());
    write64le(C.Memory.data() + OriginalTLS +
                  offsetof(coff_tls_directory64, AddressOfCallBacks),
              EmptyArray ? C.Base + Callbacks : 0);
    write64le(C.Memory.data() + Callbacks, 0);
    std::fill_n(C.Memory.data() + ProgramTLS, sizeof(coff_tls_directory64), 0);
    C.Baseline = C.Memory;
    C.Transfers = {{Linked.Entry, true, 1, true}};
  }

  static constexpr uint64_t FirstGate = 0x70010000, SecondGate = 0x70020000;
  const ExportBinding FirstBinding{"first.dll", "first", {}};
  const ExportBinding SecondBinding{"second.dll", "second", {}};
  test::Image Linked;
  std::vector<uint8_t> Bytes;
  std::unique_ptr<pe::Image> Input;
  Capture C;
  uint64_t OriginalTLS = 0, ProgramTLS = 0, Callbacks = 0, Callback = 0;
};

TEST_F(PERebuild, DLLNotificationsRequireAnExecutableOriginalEntry) {
  const uint64_t Characteristics =
      Linked.MachineOffset +
      offsetof(llvm::object::coff_file_header, Characteristics);
  for (uint16_t Machine : {uint16_t(llvm::COFF::IMAGE_FILE_MACHINE_AMD64),
                           uint16_t(llvm::COFF::IMAGE_FILE_MACHINE_ARM64)}) {
    write16le(Bytes.data() + Linked.MachineOffset, Machine);
    write16le(Bytes.data() + Characteristics,
              Linked.FileCharacteristics | llvm::COFF::IMAGE_FILE_DLL);
    prepare();
    ASSERT_FALSE(HasFailure());
    std::vector<uint8_t> Metadata;
    // An unchanged entry needs no dispatch at all.
    auto Unchanged = pe::rebuildEntry(*Input, C, Input->extent(), Metadata);
    ASSERT_TRUE(bool(Unchanged)) << llvm::toString(Unchanged.takeError());
    EXPECT_EQ(*Unchanged, C.EntryRVA);
    EXPECT_TRUE(Metadata.empty());
    C.EntryRVA += 0x100;
    C.PageAccess[Linked.Entry / 4096] = emulation::Read;
    auto Missing = pe::rebuildEntry(*Input, C, Input->extent(), Metadata);
    ASSERT_FALSE(bool(Missing));
    EXPECT_NE(llvm::toString(Missing.takeError()).find("loader notifications"),
              std::string::npos);
    EXPECT_TRUE(Metadata.empty());
    C.PageAccess[Linked.Entry / 4096] |= emulation::Execute;
    auto Available = pe::rebuildEntry(*Input, C, Input->extent(), Metadata);
    ASSERT_TRUE(bool(Available)) << llvm::toString(Available.takeError());
    EXPECT_EQ(*Available, Input->extent());
    EXPECT_FALSE(Metadata.empty());
  }
}

TEST_F(PERebuild, NewImportCellsCannotConsumeProgramZeroFill) {
  plantCall(Linked.Entry);
  const auto Before = C.Memory;
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding, false,
                                C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.RepairedTailCalls, 1u);
  ASSERT_EQ(Result.Imports.size(), 1u);
  EXPECT_GE(Result.Imports.front().SlotRVA, Input->extent());
  const auto Image = test::readImage(Result.File);
  ASSERT_FALSE(HasFailure());
  const uint64_t Next = Linked.Entry + 6;
  EXPECT_EQ(Image.Mapped[Linked.Entry], 0xff);
  EXPECT_EQ(Image.Mapped[Linked.Entry + 1], 0x15);
  EXPECT_EQ(int64_t(Next) + int32_t(read32le(Image.Mapped.data() + Next - 4)),
            Result.Imports.front().SlotRVA);
  EXPECT_TRUE(Image.Sections.back().Characteristics &
              llvm::COFF::IMAGE_SCN_MEM_WRITE);
  for (const auto &Section : Linked.Sections)
    for (uint64_t At = Section.RVA; At < Section.RVA + Section.VirtualSize;
         ++At)
      if (At < Linked.Entry || At >= Next)
        ASSERT_EQ(Image.Mapped[At], Before[At]) << llvm::utohexstr(At);
}

TEST_F(PERebuild, IATProtectionExcludesWritableAndAppendedCells) {
  ASSERT_GT(Input->regions().size(), 2u);
  const auto &Region = Input->regions()[1];
  const uint64_t First = Region.RVA + 0x100;
  const uint64_t Last = Input->regions().back().RVA + 0x100;
  write64le(C.Memory.data() + First, FirstGate);
  write64le(C.Memory.data() + First + 8, 0);
  write64le(C.Memory.data() + Last, SecondGate);
  write64le(C.Memory.data() + Last + 8, 0);
  for (uint64_t Page = Region.RVA / 4096;
       Page < (Region.RVA + Region.MemorySize) / 4096; ++Page)
    C.PageAccess[Page] = emulation::Read;
  plantCall(Linked.Entry);
  const ExportBinding Added{"third.dll", "third", {}};
  const auto Result = rebuild(
      {{C.Base + Linked.Entry + 6, Added, false, C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Imports.size(), 3u);
  const auto Image = test::readImage(Result.File);
  ASSERT_FALSE(HasFailure());
  const auto IAT = Image.directory(llvm::COFF::IAT);
  EXPECT_EQ(IAT.RelativeVirtualAddress, First);
  EXPECT_EQ(IAT.Size, 16u);
  for (const auto &Import : Result.Imports) {
    EXPECT_EQ(read64le(Image.Mapped.data() + Import.SlotRVA + 8), 0u);
    if (Import.SlotRVA != First)
      EXPECT_GE(Import.SlotRVA,
                uint64_t(IAT.RelativeVirtualAddress) + IAT.Size);
  }
  EXPECT_FALSE(Image.Sections[1].Characteristics &
               llvm::COFF::IMAGE_SCN_MEM_WRITE);
  EXPECT_TRUE(Image.Sections.back().Characteristics &
              llvm::COFF::IMAGE_SCN_MEM_WRITE);
}

TEST_F(PERebuild, WritableCellsNeedNoNativeIATProtection) {
  plantCall(Linked.Entry);
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding, false,
                                C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Imports.size(), 1u);
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(Image.directory(llvm::COFF::IAT).RelativeVirtualAddress, 0u);
  EXPECT_EQ(Image.directory(llvm::COFF::IAT).Size, 0u);
  EXPECT_TRUE(Image.Sections.back().Characteristics &
              llvm::COFF::IMAGE_SCN_MEM_WRITE);
}

TEST_F(PERebuild, IATProtectionCannotCoverExecutableOrWritablePages) {
  ASSERT_GT(Input->regions().size(), 2u);
  for (bool Executable : {false, true}) {
    SCOPED_TRACE(Executable);
    prepare();
    ASSERT_FALSE(HasFailure());
    C.Exports.emplace(FirstGate, FirstBinding);
    for (size_t I : {size_t(0), Input->regions().size() - 1}) {
      const auto &Region = Input->regions()[I];
      write64le(C.Memory.data() + Region.RVA + 0x100, FirstGate);
      write64le(C.Memory.data() + Region.RVA + 0x108, 0);
      for (uint64_t Page = Region.RVA / 4096;
           Page < (Region.RVA + Region.MemorySize) / 4096; ++Page)
        C.PageAccess[Page] =
            emulation::Read | (Executable ? emulation::Execute : 0);
    }
    auto Result = pe::rebuild(*Input, C, {});
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find(pe::text::IATProtection),
              std::string::npos);
  }
}

TEST_F(PERebuild, IATDirectoryUsesOnlyReservedOptionalHeaderSpace) {
  using llvm::object::coff_file_header;
  using llvm::object::coff_section;
  using llvm::object::pe32plus_header;
  const auto Headers = Input->headers();
  const uint64_t DirectoryCount = Headers.OptionalHeaderOffset +
                                  offsetof(pe32plus_header, NumberOfRvaAndSize);
  const uint16_t TruncatedSize =
      sizeof(pe32plus_header) +
      llvm::COFF::IAT * sizeof(llvm::object::data_directory);
  for (bool Reserved : {true, false}) {
    SCOPED_TRACE(Reserved);
    Bytes = Linked.File;
    write32le(Bytes.data() + DirectoryCount, llvm::COFF::IAT);
    if (!Reserved) {
      std::memmove(Bytes.data() + Headers.OptionalHeaderOffset + TruncatedSize,
                   Bytes.data() + Headers.SectionTableOffset,
                   Headers.Sections.size() * sizeof(coff_section));
      write16le(Bytes.data() + Headers.FileHeaderOffset +
                    offsetof(coff_file_header, SizeOfOptionalHeader),
                TruncatedSize);
    }
    prepare();
    ASSERT_FALSE(HasFailure());
    C.Exports.emplace(FirstGate, FirstBinding);
    const auto &Region = Input->regions().back();
    const uint64_t Cell = Region.RVA + 0x100;
    for (uint64_t Page = Region.RVA / 4096;
         Page < (Region.RVA + Region.MemorySize) / 4096; ++Page)
      C.PageAccess[Page] = emulation::Read;
    write64le(C.Memory.data() + Cell, FirstGate);
    write64le(C.Memory.data() + Cell + 8, 0);
    auto Result = pe::rebuild(*Input, C, {});
    if (!Reserved) {
      ASSERT_FALSE(bool(Result));
      EXPECT_NE(llvm::toString(Result.takeError()).find(pe::text::HeaderRoom),
                std::string::npos);
      continue;
    }
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    const auto Image = test::readImage(Result->File);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Image.Directories.size(), llvm::COFF::IAT + 1u);
    EXPECT_EQ(Image.directory(llvm::COFF::IAT).RelativeVirtualAddress, Cell);
    EXPECT_EQ(Image.directory(llvm::COFF::IAT).Size, 16u);
    ASSERT_EQ(Image.Sections.size(), Linked.Sections.size() + 1);
    for (size_t I = 0; I < Linked.Sections.size(); ++I)
      EXPECT_EQ(Image.Sections[I].RVA, Linked.Sections[I].RVA);
  }
}

TEST_F(PERebuild, ExportPointerWithoutAnArrayTerminatorIsNotAnImportCell) {
  const uint64_t Cell = Input->regions().back().RVA + 0x41;
  write64le(C.Memory.data() + Cell, FirstGate);
  write64le(C.Memory.data() + Cell + 8, 0x5555555555555555);
  plantCall(Linked.Entry);
  const auto Before = C.Memory;
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding, false,
                                C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Imports.size(), 1u);
  EXPECT_GE(Result.Imports.front().SlotRVA, Input->extent());
  EXPECT_EQ(Result.RepairedTailCalls, 1u);
  const auto Image = test::readImage(Result.File);
  EXPECT_TRUE(std::equal(Before.begin() + Cell, Before.begin() + Cell + 16,
                         Image.Mapped.begin() + Cell));
}

TEST_F(PERebuild, InternalExportPointersCannotCreateASelfImport) {
  const uint64_t Cell = Input->regions().back().RVA + 0x41;
  const uint64_t Pointer = C.Base + Linked.Entry;
  C.Exports.emplace(Pointer, ExportBinding{"input.dll", "Query", {}});
  write64le(C.Memory.data() + Cell, Pointer);
  write64le(C.Memory.data() + Cell + 8, 0);
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(Result.Imports.empty());
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(Image.directory(llvm::COFF::IAT).RelativeVirtualAddress, 0u);
  EXPECT_EQ(Image.directory(llvm::COFF::IAT).Size, 0u);
  EXPECT_EQ(read64le(Image.Mapped.data() + Cell), Pointer);
  EXPECT_EQ(read64le(Image.Mapped.data() + Cell + 8), 0u);
}

TEST_F(PERebuild, AdjacentProvidersCannotBorrowEachOthersImportTerminator) {
  const uint64_t Cell = Input->regions().back().RVA + 0x40;
  write64le(C.Memory.data() + Cell, FirstGate);
  write64le(C.Memory.data() + Cell + 8, SecondGate);
  write64le(C.Memory.data() + Cell + 16, 0);
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Imports.size(), 1u);
  EXPECT_EQ(Result.Imports.front().SlotRVA, Cell + 8);
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(read64le(Image.Mapped.data() + Cell), FirstGate);
}

TEST_F(PERebuild, ExistingCellIsReusedWithoutAllocatingAnotherImport) {
  plantCall(Linked.Entry);
  const uint64_t Cell = Input->regions().back().RVA + 0x100;
  write64le(C.Memory.data() + Cell, FirstGate);
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding, false,
                                C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.RepairedTailCalls, 1u);
  ASSERT_EQ(Result.Imports.size(), 1u);
  EXPECT_EQ(Result.Imports.front().SlotRVA, Cell);
}

TEST_F(PERebuild, AProvenExportIdentityDoesNotNeedTheEntryCaptureAddressMap) {
  plantCall(Linked.Entry);
  C.Exports.clear();
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding, false,
                                C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedTailCalls, 1u);
  ASSERT_EQ(Result.Imports.size(), 1u);
  EXPECT_EQ(Result.Imports.front().Module, FirstBinding.Module);
  EXPECT_EQ(Result.Imports.front().Name, FirstBinding.Name);
  EXPECT_GE(Result.Imports.front().SlotRVA, Input->extent());
}

TEST_F(PERebuild, AnIncompleteExportIdentityCannotAuthorizeRepair) {
  plantCall(Linked.Entry);
  const ExportBinding Incomplete[] = {
      {}, {"", "first", {}}, {"first.dll", "", {}}};
  for (const auto &Target : Incomplete) {
    const auto Result = rebuild(
        {{C.Base + Linked.Entry + 6, Target, false, C.Base + Linked.Entry}});
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Result.RepairedTailCalls, 0u);
    EXPECT_TRUE(Result.Imports.empty());
    const auto Image = test::readImage(Result.File);
    EXPECT_TRUE(std::equal(C.Memory.begin() + Linked.Entry,
                           C.Memory.begin() + Linked.Entry + 6,
                           Image.Mapped.begin() + Linked.Entry));
  }
}

TEST_F(PERebuild, ConflictingExportWitnessesCannotRewriteACallSite) {
  plantCall(Linked.Entry);
  const auto Result = rebuild(
      {{C.Base + Linked.Entry + 6, FirstBinding, false, C.Base + Linked.Entry},
       {C.Base + Linked.Entry + 6, SecondBinding, false, C.Base + Linked.Entry},
       {C.Base + Linked.Entry + 6, FirstBinding, false,
        C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedTailCalls, 0u);
  EXPECT_EQ(Result.ConflictingTailCalls, 1u);
  EXPECT_TRUE(Result.Imports.empty());
  const auto Image = test::readImage(Result.File);
  EXPECT_TRUE(std::equal(C.Memory.begin() + Linked.Entry,
                         C.Memory.begin() + Linked.Entry + 6,
                         Image.Mapped.begin() + Linked.Entry));
}

TEST_F(PERebuild, X64CallBytesInAnARM64ImageHaveNoRepairAuthority) {
  write16le(Bytes.data() + Linked.MachineOffset,
            llvm::COFF::IMAGE_FILE_MACHINE_ARM64);
  prepare();
  ASSERT_FALSE(HasFailure());
  C.Exports.emplace(FirstGate, FirstBinding);
  plantCall(Linked.Entry);
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding, false,
                                C.Base + Linked.Entry}});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedTailCalls, 0u);
  EXPECT_TRUE(Result.Imports.empty());
}

TEST_F(PERebuild, ObservedExportAddressLoadsUseAnOwnedImportCell) {
  const uint64_t RVA = Linked.Entry;
  const uint8_t Bytes[] = {0x5b, 0xe8, 0x20, 0, 0, 0, 0xc3};
  std::copy(std::begin(Bytes), std::end(Bytes), C.Memory.begin() + RVA);
  const auto Result =
      rebuild({{C.Base + RVA + 7, FirstBinding, true, C.Base + RVA, 3}});
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(Result.Imports.size(), 1u);
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(Image.Mapped[RVA], 0x48);
  EXPECT_EQ(Image.Mapped[RVA + 1], 0x8b);
  EXPECT_EQ(Image.Mapped[RVA + 2], 0x1d);
  EXPECT_EQ(int64_t(RVA + 7) + int32_t(read32le(Image.Mapped.data() + RVA + 3)),
            Result.Imports.front().SlotRVA);
}

TEST_F(PERebuild, ExportAddressLoadsPreserveEveryGPRDestinationExceptRSP) {
  const uint64_t RVA = Linked.Entry;
  for (unsigned Register = 0; Register != 16; ++Register) {
    SCOPED_TRACE(Register);
    const bool Extended = Register >= 8;
    uint8_t *Site = C.Memory.data() + RVA;
    const unsigned Prefix = Extended ? 1 : 0;
    if (Extended)
      Site[0] = 0x41;
    Site[Prefix] = 0x58 | (Register & 7);
    Site[Prefix + 1] = 0xe8;
    write32le(Site + Prefix + 2, 0x20);
    Site[Prefix + 6] = 0xc3;
    const auto Result = rebuild({{C.Base + RVA + Prefix + 7, FirstBinding, true,
                                  C.Base + RVA, Register}});
    ASSERT_FALSE(HasFailure());
    const auto Image = test::readImage(Result.File);
    if (Register == 4) {
      EXPECT_EQ(Result.RepairedImportLoads, 0u);
      EXPECT_TRUE(Result.Imports.empty());
      EXPECT_TRUE(std::equal(Site, Site + 7, Image.Mapped.begin() + RVA));
      continue;
    }
    EXPECT_EQ(Result.RepairedImportLoads, 1u);
    ASSERT_EQ(Result.Imports.size(), 1u);
    EXPECT_EQ(Image.Mapped[RVA], Extended ? 0x4c : 0x48);
    EXPECT_EQ(Image.Mapped[RVA + 1], 0x8b);
    EXPECT_EQ(Image.Mapped[RVA + 2], 0x05 | ((Register & 7) << 3));
    EXPECT_EQ(int64_t(RVA + 7) +
                  int32_t(read32le(Image.Mapped.data() + RVA + 3)),
              Result.Imports.front().SlotRVA);
    if (Extended)
      EXPECT_EQ(Image.Mapped[RVA + 7], 0x90);
  }
}

TEST_F(PERebuild, AddressLoadsNeedTheirExecutedStart) {
  const uint64_t RVA = Linked.Entry;
  const uint8_t Bytes[] = {0x5b, 0xe8, 0x20, 0, 0, 0, 0xc3};
  std::copy(std::begin(Bytes), std::end(Bytes), C.Memory.begin() + RVA);
  const auto Result = rebuild({{C.Base + RVA + 7, FirstBinding, true, 0, 3}});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedImportLoads, 0u);
  EXPECT_TRUE(Result.Imports.empty());
}

TEST_F(PERebuild, AddressLoadsNeedAnObservedResultRegister) {
  const uint64_t RVA = Linked.Entry;
  const uint8_t Bytes[] = {0x5b, 0xe8, 0x20, 0, 0, 0, 0xc3};
  std::copy(std::begin(Bytes), std::end(Bytes), C.Memory.begin() + RVA);
  const auto Result =
      rebuild({{C.Base + RVA + 7, FirstBinding, true, C.Base + RVA}});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedImportLoads, 0u);
  EXPECT_TRUE(Result.Imports.empty());
}

TEST_F(PERebuild, CallWitnessesNeedTheirExecutedStart) {
  plantCall(Linked.Entry);
  const auto Result = rebuild({{C.Base + Linked.Entry + 6, FirstBinding}});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedTailCalls, 0u);
  EXPECT_TRUE(Result.Imports.empty());
}

TEST_F(PERebuild, PureCallWindowsRetainTheExactAPIReturnAddress) {
  const uint64_t RVA = Linked.Entry;
  for (unsigned Size : {6u, 7u, 8u}) {
    SCOPED_TRACE(Size);
    C.Memory[RVA] = 0xe8;
    write32le(C.Memory.data() + RVA + 1, 0x20);
    C.Memory[RVA + 5] = 0x0f;
    C.Memory[RVA + 6] = 0x0b;
    C.Memory[RVA + 7] = 0xf4;
    const auto Result =
        rebuild({{C.Base + RVA + Size, FirstBinding, false, C.Base + RVA}});
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(Result.RepairedTailCalls, 1u);
    ASSERT_EQ(Result.Imports.size(), 1u);
    const auto Image = test::readImage(Result.File);
    const uint64_t Call = RVA + Size - 6;
    for (unsigned I = 0; I < Size - 6; ++I)
      EXPECT_EQ(Image.Mapped[RVA + I], 0x90);
    EXPECT_EQ(Image.Mapped[Call], 0xff);
    EXPECT_EQ(Image.Mapped[Call + 1], 0x15);
    EXPECT_EQ(int64_t(RVA + Size) +
                  int32_t(read32le(Image.Mapped.data() + Call + 2)),
              Result.Imports.front().SlotRVA);
  }
}

TEST_F(PERebuild, APreviousRexShapedByteIsNotAnExecutedAddressLoadPrefix) {
  const uint64_t RVA = Linked.Entry;
  const uint8_t Bytes[] = {0x41, 0x5b, 0xe8, 0x20, 0, 0, 0, 0xc3};
  std::copy(std::begin(Bytes), std::end(Bytes), C.Memory.begin() + RVA - 1);
  const auto Result =
      rebuild({{C.Base + RVA + 7, FirstBinding, true, C.Base + RVA, 3}});
  ASSERT_FALSE(HasFailure());
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(Result.RepairedImportLoads, 1u);
  EXPECT_EQ(Image.Mapped[RVA - 1], 0x41);
  EXPECT_EQ(Image.Mapped[RVA], 0x48);
  EXPECT_EQ(Image.Mapped[RVA + 2], 0x1d);
}

TEST_F(PERebuild, AddressLoadStartsSharingAReturnCannotOverwriteEachOther) {
  const uint64_t RVA = Linked.Entry;
  const uint8_t Bytes[] = {0x41, 0x5b, 0xe8, 0x20, 0, 0, 0, 0xc3};
  std::copy(std::begin(Bytes), std::end(Bytes), C.Memory.begin() + RVA);
  const auto Result = rebuild({
      {C.Base + RVA + 8, FirstBinding, true, C.Base + RVA, 11},
      {C.Base + RVA + 8, FirstBinding, true, C.Base + RVA + 1, 3},
  });
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedImportLoads, 0u);
  EXPECT_EQ(Result.ConflictingTailCalls, 1u);
  const auto Image = test::readImage(Result.File);
  EXPECT_TRUE(std::equal(std::begin(Bytes), std::end(Bytes),
                         Image.Mapped.begin() + RVA));
}

TEST_F(PERebuild, NestedWindowsCannotHideANonAdjacentOverlap) {
  const uint64_t RVA = Linked.Entry;
  const uint8_t Bytes[] = {0xe8, 0xe8, 0, 0, 0, 0,   0x0f,
                           0xe8, 0x20, 0, 0, 0, 0x0f};
  std::copy(std::begin(Bytes), std::end(Bytes), C.Memory.begin() + RVA);
  // [0,8) overlaps both [1,7) and [7,13), although the latter pair merely
  // touches. Adjacent-pair checks alone miss the third conflicting start.
  const auto Result = rebuild({
      {C.Base + RVA + 8, FirstBinding, false, C.Base + RVA},
      {C.Base + RVA + 7, FirstBinding, false, C.Base + RVA + 1},
      {C.Base + RVA + 13, FirstBinding, false, C.Base + RVA + 7},
  });
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedTailCalls, 0u);
  EXPECT_EQ(Result.ConflictingTailCalls, 3u);
  EXPECT_TRUE(Result.Imports.empty());
  const auto Image = test::readImage(Result.File);
  EXPECT_TRUE(std::equal(std::begin(Bytes), std::end(Bytes),
                         Image.Mapped.begin() + RVA));
}

TEST_F(PERebuild, ConflictingContinuationsRetainEveryOverlappingStart) {
  const uint64_t Short = Linked.Entry + 64;
  plantCall(Short - 1);
  plantCall(Short - 7);
  // [S,S+6) and [S-1,S+6) share a continuation. Their conflict must retain
  // the second window's PUSH prefix, which also belongs to [S-7,S).
  for (bool SameTarget : {false, true}) {
    SCOPED_TRACE(SameTarget);
    const std::array<TailImport, 3> Witnesses = {{
        {C.Base + Short + 6, FirstBinding, false, C.Base + Short},
        {C.Base + Short + 6, SameTarget ? FirstBinding : SecondBinding, false,
         C.Base + Short - 1},
        {C.Base + Short, SecondBinding, false, C.Base + Short - 7},
    }};
    std::array<unsigned, 3> Order = {0, 1, 2};
    do {
      SCOPED_TRACE(testing::PrintToString(Order));
      for (bool Repeat : {false, true}) {
        SCOPED_TRACE(Repeat);
        std::vector<TailImport> Calls;
        for (unsigned Index : Order)
          Calls.push_back(Witnesses[Index]);
        if (Repeat)
          for (unsigned Index : Order)
            Calls.push_back(Witnesses[Index]);
        const auto Result = rebuild(Calls);
        ASSERT_FALSE(HasFailure());
        EXPECT_EQ(Result.RepairedTailCalls, 0u);
        EXPECT_EQ(Result.ConflictingTailCalls, 2u);
        EXPECT_TRUE(Result.Imports.empty());
        const auto Image = test::readImage(Result.File);
        EXPECT_TRUE(std::equal(C.Memory.begin() + Short - 7,
                               C.Memory.begin() + Short + 6,
                               Image.Mapped.begin() + Short - 7));
      }
    } while (std::next_permutation(Order.begin(), Order.end()));
  }
}

TEST_F(PERebuild, TouchingWindowsAndRepeatedWitnessesRemainRepairable) {
  const uint64_t RVA = Linked.Entry;
  plantCall(RVA);
  plantCall(RVA + 6);
  const TailImport First{C.Base + RVA + 6, FirstBinding, false, C.Base + RVA};
  const TailImport Second{C.Base + RVA + 12, SecondBinding, false,
                          C.Base + RVA + 6};
  const auto Result = rebuild({First, Second, Second, First});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.RepairedTailCalls, 2u);
  EXPECT_EQ(Result.ConflictingTailCalls, 0u);
  EXPECT_EQ(Result.Imports.size(), 2u);
  const auto Image = test::readImage(Result.File);
  for (uint64_t Start : {RVA, RVA + 6}) {
    EXPECT_EQ(Image.Mapped[Start], 0xff);
    EXPECT_EQ(Image.Mapped[Start + 1], 0x15);
  }
}

TEST_F(PERebuild, UnwitnessedScannedTLSKeepsTheValidatedOriginal) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Transfers.clear();
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, OriginalTLS);
  EXPECT_NE(*Result, ProgramTLS);
}

TEST_F(PERebuild, ValidOriginalTLSWithoutWitnessesRemainsLoadableMetadata) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Transfers.clear();
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress,
            OriginalTLS);
  EXPECT_TRUE(
      std::equal(C.Memory.begin() + OriginalTLS,
                 C.Memory.begin() + OriginalTLS + sizeof(coff_tls_directory64),
                 Image.Mapped.begin() + OriginalTLS));
  EXPECT_EQ(Result.MaterializedTLSCallbacks, 0u);
}

TEST_F(PERebuild, UnwitnessedOriginalTLSRestoresStateAndKeepsPendingCallbacks) {
  for (uint16_t Machine : {llvm::COFF::IMAGE_FILE_MACHINE_AMD64,
                           llvm::COFF::IMAGE_FILE_MACHINE_ARM64}) {
    SCOPED_TRACE(Machine);
    write16le(Bytes.data() + Linked.MachineOffset, Machine);
    prepareTLS();
    ASSERT_FALSE(HasFailure());
    C.Transfers.clear();
    ASSERT_TRUE(C.Initializers.empty());
    ASSERT_TRUE(C.CompletedCalls.empty());
    (*C.ThreadLocal)[0] ^= 0x42;
    const auto Before = C.Memory;
    const auto Result = rebuild({});
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Result.MaterializedTLSCallbacks, 0u);
    const auto Image = test::readImage(Result.File);
    const uint64_t Directory =
        Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
    ASSERT_GE(Directory, Input->extent());
    coff_tls_directory64 Record;
    std::memcpy(&Record, Image.Mapped.data() + Directory, sizeof(Record));
    const uint64_t Array = uint64_t(Record.AddressOfCallBacks) - C.Base;
    const uint64_t Adapter = read64le(Image.Mapped.data() + Array) - C.Base;
    ASSERT_GE(Adapter, Input->extent());
    EXPECT_EQ(read64le(Image.Mapped.data() + Array + 8), C.Base + Linked.Entry);
    EXPECT_EQ(read64le(Image.Mapped.data() + Array + 16), 0u);
    const uint64_t State =
        read64le(Image.Mapped.data() + Adapter +
                 (Machine == llvm::COFF::IMAGE_FILE_MACHINE_AMD64 ? 32 : 72)) -
        C.Base;
    ASSERT_LE(State + C.ThreadLocal->size(), Image.Mapped.size());
    EXPECT_TRUE(std::equal(C.ThreadLocal->begin(), C.ThreadLocal->end(),
                           Image.Mapped.begin() + State));
    // State restoration must preserve the template and pending callback.
    for (const auto &R : Input->regions())
      EXPECT_TRUE(std::equal(Before.begin() + R.RVA,
                             Before.begin() + R.RVA + R.MemorySize,
                             Image.Mapped.begin() + R.RVA));
  }
}

TEST_F(PERebuild, UnwitnessedOriginalTLSRequiresAnExactLiveSnapshot) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    prepareTLS();
    ASSERT_FALSE(HasFailure());
    C.Transfers.clear();
    if (!Mutation)
      C.ThreadLocal.reset();
    else if (Mutation == 1)
      C.ThreadLocal->pop_back();
    else
      C.ThreadLocal->push_back(0);
    auto Result = pe::rebuild(*Input, C, {});
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("live TLS snapshot"),
              std::string::npos);
  }
}

TEST_F(PERebuild, InvalidOriginalTLSNeedsAWitnessedReplacement) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    prepareTLS();
    ASSERT_FALSE(HasFailure());
    C.Transfers.clear();
    auto *Record = C.Memory.data() + OriginalTLS;
    switch (Mutation) {
    case 0:
      write64le(Record + offsetof(coff_tls_directory64, AddressOfIndex), 0);
      break;
    case 1:
      write64le(Record + offsetof(coff_tls_directory64, AddressOfCallBacks),
                C.Base + Input->extent());
      break;
    case 2:
      write64le(Record + offsetof(coff_tls_directory64, EndAddressOfRawData),
                read64le(Record) - 1);
      break;
    case 3:
      write64le(Record + offsetof(coff_tls_directory64, EndAddressOfRawData),
                read64le(Record) + C.ThreadLocal->size() + 1);
      break;
    case 4: {
      const uint64_t Array =
          read64le(Record + offsetof(coff_tls_directory64, AddressOfCallBacks));
      write64le(C.Memory.data() + Array - C.Base, C.Base + Input->extent());
      break;
    }
    }
    auto Result = pe::rebuild(*Input, C, {});
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError())
                  .find("original TLS directory is invalid"),
              std::string::npos);
  }
}

TEST_F(PERebuild, AValidWitnessedTLSRecordReplacesAnInvalidOriginal) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  write64le(C.Memory.data() + OriginalTLS +
                offsetof(coff_tls_directory64, AddressOfIndex),
            0);
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  const auto Image = test::readImage(Result.File);
  EXPECT_EQ(Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress,
            ProgramTLS);
}

TEST_F(PERebuild, OriginalTLSWithoutCallbacksKeepsTheLoaderAllocation) {
  for (bool EmptyArray : {false, true}) {
    SCOPED_TRACE(EmptyArray);
    prepareTLSWithoutCallbacks(EmptyArray);
    ASSERT_FALSE(HasFailure());
    auto Directory = pe::recoverTLSDirectory(*Input, C);
    ASSERT_TRUE(bool(Directory)) << llvm::toString(Directory.takeError());
    EXPECT_EQ(*Directory, OriginalTLS);
    std::vector<uint8_t> Metadata;
    auto TLS = pe::rebuildTLS(*Input, C, Input->extent(), Metadata);
    ASSERT_TRUE(bool(TLS)) << llvm::toString(TLS.takeError());
    EXPECT_EQ(TLS->DirectoryRVA, OriginalTLS);
    EXPECT_EQ(TLS->MaterializedCallbacks, 0u);
    EXPECT_FALSE(TLS->HasCode);
    EXPECT_TRUE(Metadata.empty());
  }
}

TEST_F(PERebuild, CallbackFreeTLSRestoresCapturedBytesWithoutChangingTemplate) {
  for (uint16_t Machine : {llvm::COFF::IMAGE_FILE_MACHINE_AMD64,
                           llvm::COFF::IMAGE_FILE_MACHINE_ARM64}) {
    SCOPED_TRACE(Machine);
    write16le(Bytes.data() + Linked.MachineOffset, Machine);
    for (bool EmptyArray : {false, true}) {
      SCOPED_TRACE(EmptyArray);
      prepareTLSWithoutCallbacks(EmptyArray);
      ASSERT_FALSE(HasFailure());
      ASSERT_TRUE(C.ThreadLocal);
      (*C.ThreadLocal)[0] = 42;
      const auto Before = C.Memory;
      std::vector<uint8_t> Metadata;
      auto TLS = pe::rebuildTLS(*Input, C, Input->extent(), Metadata);
      ASSERT_TRUE(bool(TLS)) << llvm::toString(TLS.takeError());
      EXPECT_TRUE(TLS->HasCode);
      EXPECT_EQ(TLS->MaterializedCallbacks, 0u);
      const auto Result = rebuild({});
      ASSERT_FALSE(HasFailure());
      EXPECT_EQ(Result.MaterializedTLSCallbacks, 0u);
      const auto Image = test::readImage(Result.File);
      const uint64_t Directory =
          Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
      ASSERT_GE(Directory, Input->extent());
      coff_tls_directory64 Record;
      std::memcpy(&Record, Image.Mapped.data() + Directory, sizeof(Record));
      const uint64_t Array = uint64_t(Record.AddressOfCallBacks) - C.Base;
      const uint64_t Adapter = read64le(Image.Mapped.data() + Array) - C.Base;
      EXPECT_GE(Adapter, Input->extent());
      EXPECT_EQ(read64le(Image.Mapped.data() + Array + 8), 0u);
      const uint64_t State =
          read64le(
              Image.Mapped.data() + Adapter +
              (Machine == llvm::COFF::IMAGE_FILE_MACHINE_AMD64 ? 32 : 72)) -
          C.Base;
      ASSERT_LE(State + C.ThreadLocal->size(), Image.Mapped.size());
      EXPECT_TRUE(std::equal(C.ThreadLocal->begin(), C.ThreadLocal->end(),
                             Image.Mapped.begin() + State));
      // The original TLS template remains authoritative for future threads.
      for (const auto &R : Input->regions())
        EXPECT_TRUE(std::equal(Before.begin() + R.RVA,
                               Before.begin() + R.RVA + R.MemorySize,
                               Image.Mapped.begin() + R.RVA));
      EXPECT_TRUE(Image.Sections.back().Characteristics &
                  llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
    }
  }
}

TEST_F(PERebuild, CallbackFreeTLSStillRequiresAnExactLiveSnapshot) {
  for (bool Missing : {false, true}) {
    SCOPED_TRACE(Missing);
    prepareTLSWithoutCallbacks(false);
    ASSERT_FALSE(HasFailure());
    if (Missing)
      C.ThreadLocal.reset();
    else
      C.ThreadLocal->push_back(0);
    auto Result = pe::rebuild(*Input, C, {});
    ASSERT_FALSE(bool(Result));
    EXPECT_NE(llvm::toString(Result.takeError()).find("live TLS snapshot"),
              std::string::npos);
  }
}

TEST_F(PERebuild, ZeroTLSStateUsesVirtualStorageAndPreservesTheOverlay) {
  constexpr uint8_t Overlay[] = {0x41, 0, 0x99, 0xee, 0x7b};
  for (uint16_t Machine : {llvm::COFF::IMAGE_FILE_MACHINE_AMD64,
                           llvm::COFF::IMAGE_FILE_MACHINE_ARM64}) {
    SCOPED_TRACE(Machine);
    for (bool WithOverlay : {false, true}) {
      SCOPED_TRACE(WithOverlay);
      Bytes = Linked.File;
      write16le(Bytes.data() + Linked.MachineOffset, Machine);
      if (WithOverlay)
        Bytes.insert(Bytes.end(), std::begin(Overlay), std::end(Overlay));
      prepareTLSWithoutCallbacks(false);
      ASSERT_FALSE(HasFailure());
      const uint64_t Region = Input->regions().back().RVA;
      const uint64_t Template = Region + 0x800;
      constexpr uint64_t TLSSize = 0x800;
      ASSERT_LE(Template + TLSSize, C.Memory.size());
      write64le(C.Memory.data() + OriginalTLS +
                    offsetof(coff_tls_directory64, StartAddressOfRawData),
                C.Base + Template);
      write64le(C.Memory.data() + OriginalTLS +
                    offsetof(coff_tls_directory64, EndAddressOfRawData),
                C.Base + Template + TLSSize);
      std::fill_n(C.Memory.begin() + Template, TLSSize, 0);
      C.Memory[Template] = 1;
      C.Baseline = C.Memory;
      C.ThreadLocal = std::vector<uint8_t>(TLSSize, 0);
      uint64_t InputEnd = Input->headers().SizeOfHeaders;
      for (const auto &R : Input->regions())
        InputEnd = std::max(InputEnd, R.FileOffset + R.FileSize);
      const auto ExpectedOverlay = llvm::ArrayRef(Bytes).drop_front(InputEnd);

      const auto Result = rebuild({});
      ASSERT_FALSE(HasFailure());
      const auto Image = test::readImage(Result.File);
      ASSERT_FALSE(HasFailure());
      const auto &Metadata = Image.Sections.back();
      EXPECT_GT(Metadata.VirtualSize, Metadata.FileSize);
      const uint64_t OutputEnd =
          uint64_t(Metadata.FileOffset) + Metadata.FileSize;
      ASSERT_EQ(Result.File.size(), OutputEnd + ExpectedOverlay.size());
      EXPECT_TRUE(std::equal(ExpectedOverlay.begin(), ExpectedOverlay.end(),
                             Result.File.begin() + OutputEnd));

      const uint64_t Directory =
          Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
      const uint64_t Array =
          read64le(Image.Mapped.data() + Directory +
                   offsetof(coff_tls_directory64, AddressOfCallBacks)) -
          C.Base;
      const uint64_t Adapter = read64le(Image.Mapped.data() + Array) - C.Base;
      const uint64_t State =
          read64le(
              Image.Mapped.data() + Adapter +
              (Machine == llvm::COFF::IMAGE_FILE_MACHINE_AMD64 ? 32 : 72)) -
          C.Base;
      ASSERT_LE(State + TLSSize, Image.Mapped.size());
      EXPECT_GT(State + TLSSize, uint64_t(Metadata.RVA) + Metadata.FileSize);
      EXPECT_TRUE(std::all_of(Image.Mapped.begin() + State,
                              Image.Mapped.begin() + State + TLSSize,
                              [](uint8_t Byte) { return Byte == 0; }));
      EXPECT_EQ(Image.Mapped[Template], 1u);
    }
  }
}

TEST_F(PERebuild, TLSRestorationKeepsCallbacksThatHaveNotCompleted) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  (*C.ThreadLocal)[0] = 42;
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.MaterializedTLSCallbacks, 0u);
  const auto Image = test::readImage(Result.File);
  const uint64_t Directory =
      Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
  ASSERT_GE(Directory, Input->extent());
  const uint64_t Array =
      read64le(Image.Mapped.data() + Directory +
               offsetof(coff_tls_directory64, AddressOfCallBacks)) -
      C.Base;
  EXPECT_GE(read64le(Image.Mapped.data() + Array), C.Base + Input->extent());
  EXPECT_EQ(read64le(Image.Mapped.data() + Array + 8), C.Base + Callback);
  EXPECT_EQ(read64le(Image.Mapped.data() + Array + 16), 0u);
}

TEST_F(PERebuild, EmptyTLSAllocationDoesNotRequireReadableTemplateBytes) {
  for (uint64_t Begin :
       {uint64_t(0), Linked.Base, Linked.Base + Linked.Mapped.size()}) {
    SCOPED_TRACE(Begin);
    prepareTLS();
    ASSERT_FALSE(HasFailure());
    for (uint64_t RVA : {OriginalTLS, ProgramTLS}) {
      write64le(C.Memory.data() + RVA, Begin);
      write64le(C.Memory.data() + RVA + sizeof(uint64_t), Begin);
    }
    C.Baseline = C.Memory;
    C.ThreadLocal->clear();
    C.Initializers = {C.Base + Callback};
    // A zero-length range needs no readable byte, including at the image's
    // one-past-end address. Callback and index storage remain accessible.
    C.PageAccess.front() = 0;
    auto Directory = pe::recoverTLSDirectory(*Input, C);
    ASSERT_TRUE(bool(Directory)) << llvm::toString(Directory.takeError());
    EXPECT_EQ(*Directory, ProgramTLS);
    std::vector<uint8_t> Metadata;
    auto TLS = pe::rebuildTLS(*Input, C, Input->extent(), Metadata);
    ASSERT_TRUE(bool(TLS)) << llvm::toString(TLS.takeError());
    EXPECT_EQ(TLS->MaterializedCallbacks, 1u);
    EXPECT_TRUE(TLS->HasCode);
  }
}

TEST_F(PERebuild, EmptyTLSTemplateAddressMustStillBelongToTheImage) {
  for (bool BeforeImage : {false, true}) {
    SCOPED_TRACE(BeforeImage);
    prepareTLS();
    ASSERT_FALSE(HasFailure());
    // The loader accepted an empty allocation. The observed record cannot
    // move that empty range outside the image and retain its authority.
    for (uint64_t RVA : {OriginalTLS, ProgramTLS}) {
      write64le(C.Memory.data() + RVA, 0);
      write64le(C.Memory.data() + RVA + sizeof(uint64_t), 0);
    }
    C.Baseline = C.Memory;
    const uint64_t Begin =
        BeforeImage ? C.Base - 1 : C.Base + Input->extent() + 1;
    for (uint64_t RVA : {OriginalTLS, ProgramTLS}) {
      write64le(C.Memory.data() + RVA, Begin);
      write64le(C.Memory.data() + RVA + sizeof(uint64_t), Begin);
    }
    C.Initializers = {C.Base + Callback};
    auto Directory = pe::recoverTLSDirectory(*Input, C);
    ASSERT_FALSE(bool(Directory));
    EXPECT_NE(llvm::toString(Directory.takeError())
                  .find("original TLS directory is invalid"),
              std::string::npos);
  }
}

TEST_F(PERebuild, NonemptyTLSTemplateStillRequiresReadableBytes) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  for (uint64_t RVA : {OriginalTLS, ProgramTLS}) {
    write64le(C.Memory.data() + RVA, C.Base + Linked.Entry);
    write64le(C.Memory.data() + RVA + sizeof(uint64_t),
              C.Base + Linked.Entry + C.ThreadLocal->size());
  }
  C.Baseline = C.Memory;
  C.Initializers = {C.Base + Callback};
  C.PageAccess[Linked.Entry / 4096] &= ~emulation::Read;
  auto Directory = pe::recoverTLSDirectory(*Input, C);
  ASSERT_FALSE(bool(Directory));
  EXPECT_NE(llvm::toString(Directory.takeError())
                .find("original TLS directory is invalid"),
            std::string::npos);
}

TEST_F(PERebuild, ScannedTLSWithoutCallbacksDoesNotEstablishAReplacement) {
  prepareTLSWithoutCallbacks(false);
  ASSERT_FALSE(HasFailure());
  // Only the original loader directory has provenance. Copying its valid
  // allocation fields elsewhere cannot authenticate a replacement record.
  std::memcpy(C.Memory.data() + ProgramTLS, C.Memory.data() + OriginalTLS,
              sizeof(coff_tls_directory64));
  write64le(C.Memory.data() + OriginalTLS +
                offsetof(coff_tls_directory64, AddressOfIndex),
            0);
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError())
                .find("original TLS directory is invalid"),
            std::string::npos);
}

TEST_F(PERebuild, GeneratedTLSStillOutranksTheCallbackFreeOriginal) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  write64le(C.Memory.data() + OriginalTLS +
                offsetof(coff_tls_directory64, AddressOfCallBacks),
            0);
  C.Baseline = C.Memory;
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, ProgramTLS);
  std::memcpy(C.Memory.data() + ProgramTLS + sizeof(coff_tls_directory64),
              C.Memory.data() + ProgramTLS, sizeof(coff_tls_directory64));
  auto Ambiguous = pe::recoverTLSDirectory(*Input, C);
  ASSERT_FALSE(bool(Ambiguous));
  EXPECT_NE(
      llvm::toString(Ambiguous.takeError()).find("multiple TLS directories"),
      std::string::npos);
}

TEST_F(PERebuild, TLSRestorationCannotOverflowTheCallbackTable) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  for (uint64_t I = 0; I < pe::value::MaxTLSCallbacks; ++I)
    write64le(C.Memory.data() + Callbacks + I * 8, C.Base + Callback);
  write64le(C.Memory.data() + Callbacks + pe::value::MaxTLSCallbacks * 8, 0);
  (*C.ThreadLocal)[0] = 42;
  auto Result = pe::rebuild(*Input, C, {});
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("supported callback count"),
            std::string::npos);
}

TEST_F(PERebuild, UniqueTLSRecordUsesCompleteCallbackAndAllocationEvidence) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, ProgramTLS);
  const auto Rebuilt = rebuild({});
  ASSERT_FALSE(HasFailure());
  const auto Image = test::readImage(Rebuilt.File);
  EXPECT_EQ(Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress,
            ProgramTLS);
}

TEST_F(PERebuild, GeneratedCallbackRecordWinsOverCompletedLoaderInitializer) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Linked.Entry};
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, ProgramTLS);
}

TEST_F(PERebuild, GeneratedCallbackRecordWinsOverRevisitedLoaderInitializer) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Linked.Entry};
  C.Transfers.insert(C.Transfers.begin(), {Linked.Entry, false, 1, true});
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, ProgramTLS);
}

TEST_F(PERebuild, RevisitedLoaderInitializerRemainsAFallbackTLSWitness) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Linked.Entry};
  C.Transfers = {{Linked.Entry, false, 1, true}};
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, OriginalTLS);
}

TEST_F(PERebuild, OSTLSCallbackMayHaveTheProgramEntryStack) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Transfers.front().StackBalanced = true;
  C.Transfers.front().ProgramInvocation = false;
  C.Initializers = {C.Base + Callback};
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, ProgramTLS);
}

TEST_F(PERebuild, BalancedProgramInvocationIsNotACallbackWitness) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Transfers.front().StackBalanced = true;
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(*Result, OriginalTLS);
  EXPECT_NE(*Result, ProgramTLS);
}

TEST_F(PERebuild, CompletedTLSStartupGetsAnAdapterOutsideOriginalStorage) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Callback};
  const auto Before = C.Memory;
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.MaterializedTLSCallbacks, 1u);
  const auto Image = test::readImage(Result.File);
  const auto Directory =
      Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
  ASSERT_GE(Directory, Input->extent());
  coff_tls_directory64 Record;
  std::memcpy(&Record, Image.Mapped.data() + Directory, sizeof(Record));
  const uint64_t Array = uint64_t(Record.AddressOfCallBacks) - C.Base;
  const uint64_t Adapter = read64le(Image.Mapped.data() + Array) - C.Base;
  EXPECT_GE(Adapter, Input->extent());
  EXPECT_EQ(read64le(Image.Mapped.data() + Array + 8), 0u);
  EXPECT_EQ(read64le(Image.Mapped.data() + Adapter + 16), C.Base + Callback);
  for (const auto &R : Input->regions())
    EXPECT_TRUE(std::equal(Before.begin() + R.RVA,
                           Before.begin() + R.RVA + R.MemorySize,
                           Image.Mapped.begin() + R.RVA));
  EXPECT_TRUE(Image.Sections.back().Characteristics &
              llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
}

TEST_F(PERebuild, CompletedGeneratedTLSCallsRequireTheAttachABI) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    C.CompletedCalls = {{C.Base + Callback, {C.Base, 1, 0}}};
    auto &Call = C.CompletedCalls.front();
    switch (Mutation) {
    case 1:
      ++Call.Entry;
      break;
    case 2:
      ++Call.Arguments[0];
      break;
    case 3:
      Call.Arguments[1] = 2;
      break;
    case 4:
      Call.Arguments[2] = 1;
      break;
    }
    const auto Result = rebuild({});
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(Result.MaterializedTLSCallbacks, Mutation == 0 ? 1u : 0u);
  }
}

TEST_F(PERebuild, ARM64TLSAdaptersKeepTheOriginalCallbackTarget) {
  write16le(Bytes.data() + Linked.MachineOffset,
            llvm::COFF::IMAGE_FILE_MACHINE_ARM64);
  prepare();
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Callback};
  const auto Result = rebuild({});
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(Result.MaterializedTLSCallbacks, 1u);
  const auto Image = test::readImage(Result.File);
  const uint64_t Directory =
      Image.directory(llvm::COFF::TLS_TABLE).RelativeVirtualAddress;
  const uint64_t Array =
      read64le(Image.Mapped.data() + Directory + 24) - C.Base;
  const uint64_t Adapter = read64le(Image.Mapped.data() + Array) - C.Base;
  EXPECT_EQ(read32le(Image.Mapped.data() + Adapter), 0x7100043fu);
  EXPECT_EQ(read64le(Image.Mapped.data() + Adapter + 24), C.Base + Callback);
}

TEST_F(PERebuild, CompletedTLSStartupRequiresALiveThreadSnapshot) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Callback};
  C.ThreadLocal.reset();
  auto Result = pe::rebuild(*Input, C, {});
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("live TLS snapshot"),
            std::string::npos);
}

TEST_F(PERebuild, CompletedTLSStartupRejectsAMismatchedThreadSnapshot) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Initializers = {C.Base + Callback};
  C.ThreadLocal->push_back(0);
  auto Result = pe::rebuild(*Input, C, {});
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("live TLS snapshot"),
            std::string::npos);
}

TEST_F(PERebuild, AmbiguousTLSRecordsFailInsteadOfSelectingTheFirst) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  std::memcpy(C.Memory.data() + ProgramTLS + sizeof(coff_tls_directory64),
              C.Memory.data() + ProgramTLS, sizeof(coff_tls_directory64));
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_FALSE(bool(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("multiple TLS directories"),
            std::string::npos);
}

TEST_F(PERebuild, TLSCallbacksRequireTerminationAndExecutableTargets) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  const auto Before = C.Memory;
  for (uint64_t I = 0; I <= 256; ++I)
    write64le(C.Memory.data() + Callbacks + I * 8, C.Base + Callback);
  auto Unterminated = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Unterminated)) << llvm::toString(Unterminated.takeError());
  EXPECT_EQ(*Unterminated, OriginalTLS);
  EXPECT_NE(*Unterminated, ProgramTLS);
  C.Memory = Before;
  C.PageAccess[Callback / 4096] &= ~emulation::Execute;
  auto NotCode = pe::recoverTLSDirectory(*Input, C);
  ASSERT_FALSE(bool(NotCode));
  EXPECT_NE(llvm::toString(NotCode.takeError())
                .find("original TLS directory is invalid"),
            std::string::npos);
}

TEST_F(PERebuild, InvalidSnapshotSizesCannotBeUsedForTLSScanning) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Baseline.clear();
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
}
} // namespace
} // namespace neverd::unpack
