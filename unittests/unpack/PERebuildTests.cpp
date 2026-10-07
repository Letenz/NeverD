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

  static constexpr uint64_t FirstGate = 0x70010000, SecondGate = 0x70020000;
  const ExportBinding FirstBinding{"first.dll", "first", {}};
  const ExportBinding SecondBinding{"second.dll", "second", {}};
  test::Image Linked;
  std::vector<uint8_t> Bytes;
  std::unique_ptr<pe::Image> Input;
  Capture C;
  uint64_t OriginalTLS = 0, ProgramTLS = 0, Callbacks = 0, Callback = 0;
};

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

TEST_F(PERebuild, TLSAllocationIdentityNeedsAnObservedCallback) {
  prepareTLS();
  ASSERT_FALSE(HasFailure());
  C.Transfers.clear();
  auto Result = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_FALSE(*Result);
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
  EXPECT_FALSE(*Result);
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
  EXPECT_FALSE(*Unterminated);
  C.Memory = Before;
  C.PageAccess[Callback / 4096] &= ~emulation::Execute;
  auto NotCode = pe::recoverTLSDirectory(*Input, C);
  ASSERT_TRUE(bool(NotCode)) << llvm::toString(NotCode.takeError());
  EXPECT_FALSE(*NotCode);
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
