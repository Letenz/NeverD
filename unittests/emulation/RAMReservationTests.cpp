//===- RAMReservationTests.cpp - Physical reservation interference --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryLayout.h"
#include "core/RAMTransaction.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include <future>

namespace neverd::emulation {
namespace {
#define NEVERD_RAM_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_RAM_TEST_BYTES(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "RAMTransactionCases.def"
#undef NEVERD_RAM_TEST_VALUE
#undef NEVERD_RAM_TEST_BYTES

class RAMReservations : public testing::Test {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::shared_ptr<AddressSpace> Space;
  bool Running = false;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    Space = Memory->addressSpace();
    llvm::cantFail(Space->map(Data, memory::PageSize, Read | Write));
    llvm::cantFail(
        Space->mapAlias(Alias, Data, memory::PageSize, Read | Write));
  }
  void TearDown() override { stop(); }
  void start(RAMWriteTracking Tracking = RAMWriteTracking::Declared) {
    llvm::cantFail(Memory->beginRun(Tracking));
    Running = true;
  }
  void stop() {
    if (Running)
      Memory->endRun();
    Running = false;
  }
  std::shared_ptr<RAMReservation> reserve(uint64_t Address = Data) {
    start();
    auto R = llvm::cantFail(Memory->reserveRAM(Address, WordBytes, PairBytes));
    stop();
    return R;
  }
  bool matches(const std::shared_ptr<RAMReservation> &R,
               uint64_t Address = Data, uint64_t Size = WordBytes) {
    start();
    const bool Match =
        llvm::cantFail(Memory->reservationMatches(R, Address, Size));
    stop();
    return Match;
  }
};

TEST_F(RAMReservations,
       RetainsPhysicalIdentityAcrossAliasesAndPermissionChanges) {
  auto R = reserve();
  EXPECT_TRUE(matches(R));
  EXPECT_TRUE(matches(R, Alias));
  EXPECT_FALSE(matches(R, Data + WordBytes));
  EXPECT_FALSE(matches(R, Data, PairBytes));
  llvm::cantFail(Space->protect(Data, memory::PageSize, 0));
  EXPECT_TRUE(matches(R)); // Permission admission belongs to the instruction.
  llvm::cantFail(Space->unmap(Data, memory::PageSize));
  EXPECT_FALSE(matches(R));
  EXPECT_TRUE(matches(R, Alias));
  llvm::cantFail(Space->map(Data, memory::PageSize, Read | Write));
  EXPECT_FALSE(matches(R)); // Virtual reuse cannot redirect retained backing.
  EXPECT_TRUE(matches(R, Alias));
}

TEST_F(RAMReservations, SameValueAndABAHostWritesInvalidateEveryOlderWitness) {
  for (bool ABA : {false, true}) {
    llvm::cantFail(Space->writeInteger(Data, BeforeWord, WordBytes));
    auto First = reserve(), Second = reserve();
    if (ABA)
      llvm::cantFail(Space->writeInteger(Alias, AfterWord, WordBytes));
    llvm::cantFail(Space->writeInteger(Alias, BeforeWord, WordBytes));
    EXPECT_FALSE(matches(First));
    EXPECT_FALSE(matches(Second));
    auto New = reserve();
    EXPECT_TRUE(matches(New));
    EXPECT_FALSE(matches(First));
  }
}

TEST_F(RAMReservations,
       FullGranuleIsObservedButUnrelatedWritesDoNotBreakProgress) {
  for (uint64_t Offset = 0; Offset < 2 * PairBytes; ++Offset) {
    auto R = reserve(Data + WordBytes);
    llvm::cantFail(Space->writeInteger(Data + Offset, BeforeByte, 1));
    EXPECT_EQ(matches(R, Data + WordBytes), Offset >= PairBytes);
  }
  auto First = reserve(), Next = reserve(Data + PairBytes);
  llvm::cantFail(Space->writeInteger(Data + PairBytes, AfterWord, WordBytes));
  EXPECT_TRUE(matches(First));
  EXPECT_FALSE(matches(Next, Data + PairBytes));
}

TEST_F(RAMReservations, FailedHostWritesAndReadsPreserveTheWitness) {
  auto R = reserve();
  llvm::cantFail(Space->protect(Alias, memory::PageSize, Read));
  auto E = Space->writeInteger(Alias, AfterWord, WordBytes);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  auto Value = llvm::cantFail(Space->readInteger(Alias, WordBytes));
  EXPECT_EQ(Value, 0u);
  EXPECT_TRUE(matches(R));
  std::vector<uint8_t> TooLong(memory::PageSize + 1, AfterByte);
  E = Space->write(Data, TooLong);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_TRUE(matches(R));
}

TEST_F(RAMReservations, RetainedViewsAndSiblingMappingsInvalidateTheSameBytes) {
  auto View = llvm::cantFail(Space->pinBacking(Data, WordBytes));
  auto R = reserve();
  llvm::cantFail(Space->unmap(Data, memory::PageSize));
  llvm::cantFail(Space->unmapAlias(Alias, memory::PageSize));
  std::array<uint8_t, WordBytes> Bytes{};
  llvm::cantFail(View.write(0, Bytes));
  auto Sibling =
      llvm::cantFail(AddressSpace::create(Space->physicalMemory(), Limit));
  llvm::cantFail(Sibling->mapRegion(Data, View.slices()[0].Region, 0,
                                    memory::PageSize, Read | Write));
  auto Other = llvm::cantFail(MemoryProjection::create(Sibling));
  llvm::cantFail(Other->beginRun(RAMWriteTracking::Declared));
  EXPECT_FALSE(llvm::cantFail(Other->reservationMatches(R, Data, WordBytes)));
  auto Next = llvm::cantFail(Other->reserveRAM(Data, WordBytes, PairBytes));
  Other->endRun();
  llvm::cantFail(View.write(0, Bytes));
  llvm::cantFail(Other->beginRun(RAMWriteTracking::Declared));
  EXPECT_FALSE(
      llvm::cantFail(Other->reservationMatches(Next, Data, WordBytes)));
  Other->endRun();
}

TEST_F(RAMReservations,
       TemporaryNativeWritesAndRollbackDoNotPublishInterference) {
  for (bool Stage : {false, true}) {
    auto R = reserve();
    start();
    {
      const RAMWriteRange W{Alias, WordBytes};
      auto T = llvm::cantFail(RAMTransaction::create(*Memory, W, WordBytes));
      *Memory->physicalPointer(Memory->mappings().at(Alias).Physical) =
          AfterByte;
      if (Stage)
        llvm::cantFail(T->stage());
    }
    stop();
    EXPECT_TRUE(matches(R));
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, WordBytes)), 0u);
  }
}

TEST_F(RAMReservations, CommitInvalidatesEvenWhenTheBytesDidNotChange) {
  auto R = reserve();
  start();
  const RAMWriteRange W{Alias, WordBytes};
  auto T = llvm::cantFail(RAMTransaction::create(*Memory, W, WordBytes));
  llvm::cantFail(T->stage());
  EXPECT_TRUE(llvm::cantFail(Memory->reservationMatches(R, Data, WordBytes)));
  llvm::cantFail(T->commit());
  EXPECT_FALSE(llvm::cantFail(Memory->reservationMatches(R, Data, WordBytes)));
  T.reset();
  stop();
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Data, WordBytes)), 0u);
}

TEST_F(RAMReservations, OpaqueExecutionInvalidatesBeforeUntrackedRAMAccess) {
  auto R = reserve();
  start(RAMWriteTracking::Opaque);
  auto Result = Memory->reserveRAM(Data, WordBytes, PairBytes);
  ASSERT_FALSE(bool(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()),
            diagnostic::RAMReservationTracking);
  stop();
  EXPECT_FALSE(matches(R));
}

TEST_F(RAMReservations, RejectsForeignOwnersInvalidRangesAndMissingLeases) {
  auto R = reserve();
  auto Result = Memory->reserveRAM(Data, WordBytes, PairBytes);
  ASSERT_FALSE(bool(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()),
            diagnostic::RAMTransactionLease);
  auto Other = llvm::cantFail(MemoryProjection::create(Limit));
  llvm::cantFail(Other->map(Data, memory::PageSize, Read | Write));
  llvm::cantFail(Other->beginRun(RAMWriteTracking::Declared));
  EXPECT_FALSE(llvm::cantFail(Other->reservationMatches(R, Data, WordBytes)));
  Other->endRun();
  start();
  for (auto [Address, Size, Granule] :
       {std::array<uint64_t, 3>{Data, 0, PairBytes},
        {Data, WordBytes, 0},
        {Data, WordBytes, PairBytes - 1},
        {Data, WordBytes, 2 * memory::PageSize},
        {Data + PairBytes - 1, WordBytes, PairBytes},
        {UINT64_MAX, WordBytes, PairBytes},
        {Unmapped, WordBytes, PairBytes}}) {
    auto Invalid = Memory->reserveRAM(Address, Size, Granule);
    EXPECT_FALSE(bool(Invalid));
    if (!Invalid)
      llvm::consumeError(Invalid.takeError());
  }
  auto Foreign = std::async(std::launch::async, [&] {
    auto Match = Memory->reservationMatches(R, Data, WordBytes);
    if (Match)
      return std::string();
    return llvm::toString(Match.takeError());
  });
  EXPECT_EQ(Foreign.get(), diagnostic::Running);
}

struct WriterProfile {
  ExecutionBackendKind Backend;
};
void PrintTo(const WriterProfile &P, std::ostream *OS) {
  *OS << executionBackendName(P.Backend);
}
class RAMReservationWriters : public testing::TestWithParam<WriterProfile> {};

TEST_P(RAMReservationWriters,
       ScalarStringAndFaultingFrameWritesPublishInterference) {
  for (auto Bytes : {llvm::ArrayRef(StoreX64), llvm::ArrayRef(StringStoreX64),
                     llvm::ArrayRef(FaultingEnterX64)}) {
    for (bool Cancel : {false, true}) {
      auto Projection = llvm::cantFail(MemoryProjection::create(Limit));
      auto Space = Projection->addressSpace();
      llvm::cantFail(Space->map(Data, memory::PageSize, Read | Write));
      llvm::cantFail(
          Space->map(Code, memory::PageSize, Read | Write | Execute));
      llvm::cantFail(Space->write(Code, Bytes));
      auto Created = createExecutionBackend(GetParam().Backend,
                                            ExecutionContract::CheckedX64,
                                            Space, GuestArchitecture::X64);
      if (!Created) {
        auto E = Created.takeError();
        bool Unavailable = E.isA<BackendUnavailableError>();
        auto Text = llvm::toString(std::move(E));
        if (Unavailable)
          GTEST_SKIP() << Text;
        FAIL() << Text;
      }
      auto &CPU = *Created->CPU;
      llvm::cantFail(CPU.setReg(X64Register::CX, Data));
      llvm::cantFail(CPU.setReg(X64Register::DI, Data));
      llvm::cantFail(CPU.setReg(X64Register::SP, Data + WordBytes));
      llvm::cantFail(CPU.setReg(X64Register::BP, Unmapped + WordBytes));
      llvm::cantFail(CPU.setReg(X64Register::AX, BeforeWord));
      llvm::cantFail(Projection->beginRun(RAMWriteTracking::Declared));
      auto Reservation =
          llvm::cantFail(Projection->reserveRAM(Data, WordBytes, PairBytes));
      Projection->endRun();
      BackendHooks Hooks;
      Hooks.Instruction = [&](uint64_t PC, uint32_t) {
        if (PC != Code)
          CPU.stop();
      };
      Hooks.Write = [&](uint64_t, uint32_t, uint64_t) {
        if (Cancel)
          CPU.stop();
      };
      llvm::cantFail(CPU.installHooks(std::move(Hooks)));
      auto Exit = llvm::cantFail(CPU.runUntilExit(Code, Timeout));
      EXPECT_EQ(Exit.Kind, !Cancel && Bytes == llvm::ArrayRef(FaultingEnterX64)
                               ? ExecutionExitKind::GuestFault
                               : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      llvm::cantFail(Projection->beginRun(RAMWriteTracking::Declared));
      EXPECT_EQ(llvm::cantFail(Projection->reservationMatches(Reservation, Data,
                                                              WordBytes)),
                Cancel);
      Projection->endRun();
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Transports, RAMReservationWriters,
    testing::Values(WriterProfile{ExecutionBackendKind::Unicorn},
                    WriterProfile{ExecutionBackendKind::KVM},
                    WriterProfile{ExecutionBackendKind::WHP}),
    [](const testing::TestParamInfo<WriterProfile> &P) {
      return executionBackendName(P.param.Backend);
    });
} // namespace
} // namespace neverd::emulation
