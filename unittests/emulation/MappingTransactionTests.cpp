//===- MappingTransactionTests.cpp - Atomic stopped RAM publication
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic stopped RAM publication.
///

#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include <thread>

using namespace neverd::emulation;
namespace {
constexpr uint64_t Page = 4096, Code = 0x10000, Data = 0x20000;
std::shared_ptr<AddressSpace> space(uint64_t Physical = 16 * Page,
                                    uint64_t Virtual = 16 * Page) {
  return llvm::cantFail(AddressSpace::create(
      llvm::cantFail(PhysicalMemory::create(Physical)), Virtual));
}
TEST(MappingTransaction, AbandonRestoresCapacityAndLeavesOriginalLayout) {
  auto A = space();
  llvm::cantFail(A->map(Data, Page, Read | Write));
  llvm::cantFail(A->writeInteger(Data, 73, 4));
  const auto Generation = A->mappingGeneration();
  std::vector<uint8_t> Bytes(Page, 42);
  {
    auto T = llvm::cantFail(A->prepareMappings({{Code, Read, Bytes}}));
    EXPECT_FALSE(*A->canAccess(Code, 1, Read));
    EXPECT_EQ(*A->readInteger(Data, 4), 73u);
    EXPECT_EQ(A->mappedBytes(), Page);
    EXPECT_EQ(A->physicalMemory()->allocatedBytes(), 2 * Page);
    EXPECT_NE(llvm::toString(A->writeInteger(Data, 9, 4)), "");
    EXPECT_NE(llvm::toString(A->map(Data + Page, Page, Read)), "");
    auto Nested = A->prepareMappings({{Code, Read, Bytes}});
    ASSERT_FALSE(bool(Nested));
    llvm::consumeError(Nested.takeError());
    bool OtherThreadRejected = false;
    std::thread Other([&] {
      auto Layout = A->mappings();
      OtherThreadRejected = !Layout;
      if (!Layout)
        llvm::consumeError(Layout.takeError());
    });
    Other.join();
    EXPECT_TRUE(OtherThreadRejected);
  }
  EXPECT_EQ(A->physicalMemory()->allocatedBytes(), Page);
  EXPECT_EQ(A->mappingGeneration(), Generation);
  EXPECT_EQ(*A->readInteger(Data, 4), 73u);
  EXPECT_EQ(llvm::toString(A->writeInteger(Data, 9, 4)), "");
}
TEST(MappingTransaction, CommitPublishesAllBytesWithFinalRightsOnce) {
  auto A = space();
  const auto Generation = A->mappingGeneration();
  std::vector<uint8_t> First(Page, 42), Second(Page, 73);
  {
    auto T = llvm::cantFail(A->prepareMappings(
        {{Code, Read | Execute, First}, {Data, Read | Write, Second}}));
    ASSERT_EQ(llvm::toString(T->commit()), "");
    EXPECT_EQ(A->mappingGeneration(), Generation + 1);
    EXPECT_EQ(A->mappedBytes(), 2 * Page);
    EXPECT_TRUE(*A->canAccess(Code, Page, Read | Execute));
    EXPECT_FALSE(*A->canAccess(Code, 1, Write));
    EXPECT_FALSE(*A->canAccess(Data, 1, Execute));
    EXPECT_EQ(*A->readInteger(Code, 1), 42u);
    EXPECT_EQ(*A->readInteger(Data, 1), 73u);
    EXPECT_NE(llvm::toString(A->writeInteger(Data, 1, 1)), "");
    EXPECT_NE(llvm::toString(T->commit()), "");
    EXPECT_EQ(A->mappingGeneration(), Generation + 1);
    EXPECT_EQ(*A->readInteger(Data, 1), 73u);
  }
  EXPECT_EQ(llvm::toString(A->writeInteger(Data, 1, 1)), "");
}
TEST(MappingTransaction, AllocationFailureReleasesEarlierReservations) {
  auto A = space(3 * Page);
  llvm::cantFail(A->map(Data, Page, Read | Write));
  const auto Generation = A->mappingGeneration();
  std::vector<uint8_t> One(Page), Two(2 * Page);
  auto Failed =
      A->prepareMappings({{Code, Read, One}, {Data + Page, Read, Two}});
  ASSERT_FALSE(bool(Failed));
  auto E = Failed.takeError();
  EXPECT_TRUE(E.isA<GuestMemoryLimitError>());
  llvm::consumeError(std::move(E));
  EXPECT_EQ(A->physicalMemory()->allocatedBytes(), Page);
  EXPECT_EQ(A->mappedBytes(), Page);
  EXPECT_EQ(A->mappingGeneration(), Generation);
  EXPECT_FALSE(*A->canAccess(Code, 1, Read));
}
TEST(MappingTransaction, InvalidBatchesHaveNoEffects) {
  auto A = space(16 * Page, 2 * Page);
  llvm::cantFail(A->map(Data, Page, Read));
  const auto Generation = A->mappingGeneration();
  std::vector<uint8_t> One(Page), Two(2 * Page), Bad(Page - 1);
  const std::vector<std::vector<InitializedMapping>> Batches = {
      {},
      {{Code + 1, Read, One}},
      {{Code, Read, Bad}},
      {{Code, GuestPermissionMask + 1, One}},
      {{Code, Read, One}, {Code, Read, One}},
      {{Code, Read, One}, {Data, Read, One}},
      {{Code, Read, Two}},
      {{UINT64_MAX - Page + 2, Read, One}}};
  for (const auto &Batch : Batches) {
    auto Failed = A->prepareMappings(Batch);
    ASSERT_FALSE(bool(Failed));
    llvm::consumeError(Failed.takeError());
    EXPECT_EQ(A->mappingGeneration(), Generation);
    EXPECT_EQ(A->mappedBytes(), Page);
    EXPECT_EQ(A->physicalMemory()->allocatedBytes(), Page);
  }
}
TEST(MappingTransaction, DeviceCollisionPreservesDeviceAndReservations) {
  auto A = space();
  GuestMMIOCallbacks Device;
  Device.Validate = [](uint64_t, uint64_t, bool) {
    return llvm::Error::success();
  };
  Device.Read = [](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    return 7;
  };
  Device.Write = [](uint64_t, unsigned, uint64_t) {
    return llvm::Error::success();
  };
  llvm::cantFail(A->mapMMIO(Data, Page, std::move(Device)));
  const auto Generation = A->mappingGeneration();
  std::vector<uint8_t> Bytes(Page);
  auto Failed = A->prepareMappings({{Code, Read, Bytes}, {Data, Read, Bytes}});
  ASSERT_FALSE(bool(Failed));
  llvm::consumeError(Failed.takeError());
  EXPECT_EQ(A->mappingGeneration(), Generation);
  EXPECT_EQ(A->physicalMemory()->allocatedBytes(), 0u);
  ASSERT_EQ(llvm::cantFail(A->mappings()).size(), 1u);
  EXPECT_TRUE(llvm::cantFail(A->mappings())[0].Device);
  const uint8_t Patch[] = {42};
  auto Patched = A->prepareMappings({{Code, Read, Bytes}}, {{Data, Patch}});
  ASSERT_FALSE(bool(Patched));
  llvm::consumeError(Patched.takeError());
  EXPECT_EQ(A->mappingGeneration(), Generation);
  EXPECT_EQ(A->physicalMemory()->allocatedBytes(), 0u);
}
TEST(MappingTransaction, ExistingCPUProjectsNewCodeAfterStoppedCommit) {
  auto Available = llvm::cantFail(queryExecutionBackendBuild(
      ExecutionBackendKind::Unicorn, GuestArchitecture::X64));
  if (Available.Availability != BackendAvailability::Available)
    GTEST_SKIP() << Available.Reason;
  for (const auto Contract :
       {ExecutionContract::Software, ExecutionContract::CheckedX64}) {
    auto A = space();
    auto Selected = llvm::cantFail(
        createExecutionBackend(ExecutionBackendKind::Unicorn, Contract, A));
    auto &CPU = *Selected.CPU;
    llvm::cantFail(CPU.map(Code, Page, Read | Write | Execute));
    const uint8_t First[] = {0xB8, 42, 0, 0, 0, 0x90};
    llvm::cantFail(CPU.write(Code, First));
    BackendHooks Hooks;
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC == Code + 5 || PC == Data + 5)
        CPU.stop();
    };
    llvm::cantFail(CPU.installHooks(std::move(Hooks)));
    ASSERT_EQ(llvm::toString(CPU.run(Code, 1000000)), "");
    ASSERT_FALSE(CPU.fault());
    EXPECT_EQ(*CPU.reg(X64Register::AX), 42u);
    std::vector<uint8_t> Bytes(Page, 0);
    Bytes[0] = 0xB8;
    Bytes[1] = 73;
    Bytes[5] = 0x90;
    {
      auto T =
          llvm::cantFail(A->prepareMappings({{Data, Read | Execute, Bytes}}));
      EXPECT_NE(llvm::toString(CPU.run(Code, 1000000)), "");
      EXPECT_FALSE(CPU.fault());
      ASSERT_EQ(llvm::toString(T->commit()), "");
    }
    ASSERT_EQ(llvm::toString(CPU.run(Data, 1000000)), "");
    ASSERT_FALSE(CPU.fault());
    EXPECT_EQ(*CPU.reg(X64Register::AX), 73u);
    EXPECT_FALSE(*A->canAccess(Data, 1, Write));
    const uint8_t Updated[] = {42};
    {
      auto T = llvm::cantFail(A->prepareMappings({}, {{Data + 1, Updated}}));
      ASSERT_EQ(llvm::toString(T->commit()), "");
    }
    ASSERT_EQ(llvm::toString(CPU.run(Data, 1000000)), "");
    EXPECT_EQ(*CPU.reg(X64Register::AX), 42u);
  }
}
TEST(MappingTransaction, MetadataPatchesPreserveAliasesAndSnapshotTheirInputs) {
  auto A = space();
  ASSERT_EQ(llvm::toString(A->map(Data, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Data + Page, Data, Page, Read)), "");
  ASSERT_EQ(llvm::toString(A->writeInteger(Data, 73, 1)), "");
  const auto View = llvm::cantFail(A->pinBacking(Data, 1));
  const auto Generation = A->mappingGeneration();
  std::vector<uint8_t> NewPage(Page, 9), Bytes{42};
  {
    auto T = llvm::cantFail(
        A->prepareMappings({{Code, Read, NewPage}}, {{Data + Page, Bytes}}));
    EXPECT_EQ(*A->readInteger(Data, 1), 73u);
    EXPECT_FALSE(*A->canAccess(Code, 1, Read));
    Bytes[0] = 99;
    ASSERT_EQ(llvm::toString(T->commit()), "");
  }
  EXPECT_EQ(*A->readInteger(Data, 1), 42u);
  EXPECT_EQ(*A->readInteger(Data + Page, 1), 42u);
  EXPECT_EQ(*A->readInteger(Code, 1), 9u);
  EXPECT_TRUE(View.describesSameBytes(llvm::cantFail(A->pinBacking(Data, 1))));
  EXPECT_EQ(A->mappingGeneration(), Generation + 1);
}
TEST(MappingTransaction, AbandonedPatchLeavesBytesAndReservationWitnessValid) {
  auto A = space();
  ASSERT_EQ(llvm::toString(A->map(Data, Page, Read | Write)), "");
  auto Projection = llvm::cantFail(MemoryProjection::create(A));
  ASSERT_EQ(llvm::toString(Projection->beginRun(RAMWriteTracking::Declared)),
            "");
  auto Reservation = llvm::cantFail(Projection->reserveRAM(Data, 4, 4));
  Projection->endRun();
  const uint8_t Value[] = {42};
  {
    auto T = llvm::cantFail(A->prepareMappings({}, {{Data, Value}}));
    EXPECT_TRUE(Reservation->Valid);
  }
  EXPECT_TRUE(Reservation->Valid);
  EXPECT_EQ(*A->readInteger(Data, 1), 0u);
  {
    auto T = llvm::cantFail(A->prepareMappings({}, {{Data, Value}}));
    ASSERT_EQ(llvm::toString(T->commit()), "");
  }
  EXPECT_FALSE(Reservation->Valid);
  EXPECT_EQ(*A->readInteger(Data, 1), 42u);
}
TEST(MappingTransaction,
     MetadataLeasePreservesRamAliasesAndReservationWitness) {
  auto A = space();
  llvm::cantFail(A->map(Data, Page, Read | Write));
  llvm::cantFail(A->mapAlias(Data + Page, Data, Page, Read));
  llvm::cantFail(A->writeInteger(Data, 73, 4));
  auto View = llvm::cantFail(A->pinBacking(Data, 4));
  auto Projection = llvm::cantFail(MemoryProjection::create(A));
  llvm::cantFail(Projection->beginRun(RAMWriteTracking::Declared));
  auto Reservation = llvm::cantFail(Projection->reserveRAM(Data, 4, 4));
  auto Running = A->prepareMetadataPublication();
  ASSERT_FALSE(bool(Running));
  llvm::consumeError(Running.takeError());
  Projection->endRun();
  const auto Generation = A->mappingGeneration();
  for (bool Commit : {false, true}) {
    auto T = llvm::cantFail(A->prepareMetadataPublication());
    EXPECT_NE(llvm::toString(A->writeInteger(Data, 9, 4)), "");
    EXPECT_NE(llvm::toString(Projection->beginRun(RAMWriteTracking::Declared)),
              "");
    auto Nested = A->prepareMetadataPublication();
    ASSERT_FALSE(bool(Nested));
    llvm::consumeError(Nested.takeError());
    bool Rejected = false;
    std::thread Other([&] {
      auto P = A->prepareMetadataPublication();
      Rejected = !P;
      if (!P)
        llvm::consumeError(P.takeError());
    });
    Other.join();
    EXPECT_TRUE(Rejected);
    if (Commit) {
      EXPECT_EQ(llvm::toString(T->commit()), "");
      EXPECT_NE(llvm::toString(T->commit()), "");
    }
    T.reset();
    EXPECT_TRUE(Reservation->Valid);
    EXPECT_EQ(A->mappingGeneration(), Generation);
    EXPECT_EQ(A->mappedBytes(), 2 * Page);
    EXPECT_EQ(A->physicalMemory()->allocatedBytes(), Page);
    EXPECT_EQ(*A->readInteger(Data + Page, 4), 73u);
    EXPECT_TRUE(
        View.describesSameBytes(llvm::cantFail(A->pinBacking(Data, 4))));
  }
}
TEST(MappingTransaction, PatchPreflightRejectsHolesAndPhysicalAliasConflicts) {
  auto A = space();
  ASSERT_EQ(llvm::toString(A->map(Data, Page, Read | Write)), "");
  ASSERT_EQ(llvm::toString(A->mapAlias(Data + 2 * Page, Data, Page, Read)), "");
  const auto Generation = A->mappingGeneration();
  const uint8_t Value[] = {42, 73};
  std::vector<uint8_t> PageBytes(Page);
  const std::vector<std::vector<BackingPatch>> Cases = {
      {{Data, {}}},
      {{Data + Page - 1, Value}},
      {{Data, Value}, {Data + 2 * Page + 1, Value}},
      {{UINT64_MAX, Value}}};
  for (const auto &Patches : Cases) {
    auto T = A->prepareMappings({{Code, Read, PageBytes}}, Patches);
    ASSERT_FALSE(bool(T));
    llvm::consumeError(T.takeError());
    EXPECT_EQ(A->mappingGeneration(), Generation);
    EXPECT_EQ(A->physicalMemory()->allocatedBytes(), Page);
    EXPECT_EQ(*A->readInteger(Data, 2), 0u);
    EXPECT_FALSE(*A->canAccess(Code, 1, Read));
  }
}
TEST(MappingTransaction, PatchesMayInitializeNewRamInTheSameTransaction) {
  auto A = space();
  std::vector<uint8_t> Bytes(Page, 0);
  const uint8_t Value[] = {42};
  {
    auto T = llvm::cantFail(
        A->prepareMappings({{Code, Read, Bytes}}, {{Code + 1, Value}}));
    ASSERT_EQ(llvm::toString(T->commit()), "");
  }
  EXPECT_EQ(*A->readInteger(Code + 1, 1), 42u);
  EXPECT_FALSE(*A->canAccess(Code, 1, Write));
}
} // namespace
