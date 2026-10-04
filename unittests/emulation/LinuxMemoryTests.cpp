//===- LinuxMemoryTests.cpp - Process mapping and raw syscall semantics ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxMemory.h"

namespace neverd::emulation {
namespace {
#define NEVERD_LINUX_MEMORY_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "fixtures/LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_VALUE
using linux_model::ServiceKind;

class LinuxMemory : public testing::Test {
protected:
  std::shared_ptr<PhysicalMemory> RAM;
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<linux_model::LinuxMemory> Memory;
  ProcessResult Report{ProcessProfile::LinuxELF64,
                       GuestArchitecture::X64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  void SetUp() override {
    RAM = llvm::cantFail(PhysicalMemory::create(UnitLimit));
    Space = llvm::cantFail(AddressSpace::create(RAM, UnitLimit));
    ProcessOptions Options;
    Options.MemoryLimit = UnitLimit;
    Options.StackSize = PageSize;
    const linux_model::MemoryLayout Layout{UserLimit, PageSize};
    Memory = std::make_unique<linux_model::LinuxMemory>(*Space, Layout,
                                                        UnitHeap, Options);
  }
  std::optional<uint64_t> call(ServiceKind Kind, uint64_t A0 = 0,
                               uint64_t A1 = 0, uint64_t A2 = 0,
                               uint64_t A3 = 0, uint64_t A4 = 0,
                               uint64_t A5 = 0) {
    return llvm::cantFail(
        Memory->handle(Kind, {0, 0, {A0, A1, A2, A3, A4, A5}, {}}, Report));
  }
  uint64_t map(uint64_t Size, uint64_t Hint = UnitHint) {
    return *call(ServiceKind::Mmap, Hint, Size, ProtRead | ProtWrite,
                 PrivateAnonymous, UINT64_MAX);
  }
};

TEST_F(LinuxMemory, RoundedPagesAreZeroAndPartialUnmapReclaimsTheirOwners) {
  ASSERT_EQ(map(RegionPages * PageSize - 1), UnitHint);
  EXPECT_EQ(RAM->allocatedBytes(), RegionPages * PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint, sizeof(uint64_t)), 0u);
  ASSERT_EQ(llvm::toString(Space->writeInteger(UnitHint + PageSize, FirstByte,
                                               sizeof(uint64_t))),
            "");
  const auto Snapshot = llvm::cantFail(Space->mappings());
  ASSERT_EQ(Snapshot.size(), 1u);
  EXPECT_EQ(Snapshot.front().Size, RegionPages * PageSize);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), (RegionPages - 1) * PageSize);
  EXPECT_EQ(llvm::cantFail(Space->mappings()).size(), 2u);
  ASSERT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint + PageSize, sizeof(uint64_t)), 0u);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, RegionPages * PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, RegionPages * PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  EXPECT_TRUE(llvm::cantFail(Space->mappings()).empty());
  // A metadata snapshot cannot pin RAM or resurrect the retired mappings.
  EXPECT_EQ(Snapshot.front().Size, RegionPages * PageSize);
}

TEST_F(LinuxMemory, PermissionChangeKeepsTheCommittedPrefixBeforeAHole) {
  ASSERT_EQ(map(RegionPages * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_EQ(
      call(ServiceKind::Mprotect, UnitHint, RegionPages * PageSize, ProtRead),
      uint64_t(0) - NoMemory);
  EXPECT_FALSE(*Space->canAccess(UnitHint, PageSize, Write));
  EXPECT_TRUE(*Space->canAccess(UnitHint, PageSize, Read | UserAccessible));
  EXPECT_TRUE(*Space->canAccess(UnitHint + 2 * PageSize, PageSize, Write));
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint, 1, 0), 0u);
  EXPECT_TRUE(*Space->canAccess(UnitHint, PageSize, UserAccessible));
  EXPECT_FALSE(*Space->canAccess(UnitHint, 1, Read));
  EXPECT_EQ(
      call(ServiceKind::Mprotect, UnitHint, PageSize, ProtRead | ProtWrite),
      0u);
  const auto Generation = Space->mappingGeneration();
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint + 1, PageSize, ProtRead),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
}

TEST_F(LinuxMemory,
       AllocationFailurePublishesNothingAndFreedCapacityIsReusable) {
  ASSERT_EQ(map(UnitLimit), UnitHint);
  const auto Generation = Space->mappingGeneration();
  EXPECT_EQ(map(PageSize), uint64_t(0) - NoMemory);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  const auto After = Space->mappingGeneration();
  EXPECT_EQ(map(2 * PageSize), uint64_t(0) - NoMemory);
  EXPECT_EQ(Space->mappingGeneration(), After);
  EXPECT_EQ(RAM->allocatedBytes(), UnitLimit - PageSize);
  EXPECT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_EQ(RAM->allocatedBytes(), UnitLimit);
}

TEST_F(LinuxMemory, PlacementQueriesActualMappingsAndReservesStackGuards) {
  ASSERT_EQ(llvm::toString(Space->map(UnitHint, PageSize, Read | Write)), "");
  ASSERT_EQ(llvm::toString(
                Space->writeInteger(UnitHint, FirstByte, sizeof(uint64_t))),
            "");
  EXPECT_NE(map(PageSize), UnitHint);
  EXPECT_EQ(*Space->readInteger(UnitHint, sizeof(uint64_t)), FirstByte);
  ASSERT_EQ(llvm::toString(Space->unmap(UnitHint, PageSize)), "");
  EXPECT_EQ(map(PageSize), UnitHint);
  const uint64_t Guard = linux_model::StackTop - 2 * PageSize;
  EXPECT_NE(map(PageSize, Guard), Guard);
  EXPECT_FALSE(*Space->canAccess(Guard, PageSize, 0));
}

TEST_F(LinuxMemory, RawBreakRetainsItsOldValueOnCollisionAndAllocationFailure) {
  EXPECT_EQ(call(ServiceKind::Brk), UnitHeap);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + HeapOffset),
            UnitHeap + HeapOffset);
  ASSERT_EQ(llvm::toString(
                Space->writeInteger(UnitHeap, FirstByte, sizeof(uint64_t))),
            "");
  EXPECT_EQ(map(PageSize, UnitHeap + 3 * PageSize), UnitHeap + 3 * PageSize);
  const uint64_t End = UnitHeap + 2 * PageSize;
  EXPECT_EQ(call(ServiceKind::Brk, End), End);
  EXPECT_EQ(call(ServiceKind::Brk, End + 1), End);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap - 1), End);
  EXPECT_EQ(call(ServiceKind::Brk, UINT64_MAX), End);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * UnitLimit), End);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + HeapOffset),
            UnitHeap + HeapOffset);
  EXPECT_EQ(*Space->readInteger(UnitHeap, sizeof(uint64_t)), FirstByte);
  EXPECT_EQ(call(ServiceKind::Brk, End), End);
  EXPECT_EQ(*Space->readInteger(UnitHeap + PageSize, sizeof(uint64_t)), 0u);
}

TEST_F(LinuxMemory, BreakShrinkCannotReportRemovingAnAlreadyUnmappedTail) {
  const uint64_t End = UnitHeap + 3 * PageSize;
  ASSERT_EQ(call(ServiceKind::Brk, End), End);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHeap + PageSize, 2 * PageSize), 0u);
  EXPECT_EQ(call(ServiceKind::Brk, UnitHeap + HeapOffset), End);
  EXPECT_EQ(call(ServiceKind::Brk), End);
}

TEST_F(LinuxMemory, UnsupportedMappingContractsHaveNoReturnOrMemoryEffects) {
  const auto Generation = Space->mappingGeneration();
  EXPECT_FALSE(call(ServiceKind::Mmap, UnitHint, PageSize, ProtRead | ProtWrite,
                    PrivateAnonymous | MapFixed));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_FALSE(Report.Diagnostic.empty());
  EXPECT_FALSE(
      call(ServiceKind::Mmap, UnitHint, PageSize, ProtWrite, PrivateAnonymous));
  EXPECT_FALSE(call(ServiceKind::Mprotect, UnitHint, PageSize, ProtExecute));
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
}

TEST_F(LinuxMemory, EmptyAndOverflowingRangesFollowTheirOwnSystemCallRules) {
  EXPECT_EQ(map(0), uint64_t(0) - InvalidArgument);
  EXPECT_EQ(map(UINT64_MAX), uint64_t(0) - NoMemory);
  EXPECT_EQ(call(ServiceKind::Mmap, 0, PageSize, ProtRead, PrivateAnonymous,
                 UINT64_MAX, 1),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Mprotect, 0, 0, ProtRead), 0u);
  EXPECT_EQ(call(ServiceKind::Mprotect, 1, 0, ProtRead),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Mprotect, UnitHint, UINT64_MAX, ProtRead),
            uint64_t(0) - NoMemory);
  EXPECT_EQ(call(ServiceKind::Munmap, 0, 0), uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Munmap, UserLimit, PageSize),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, UINT64_MAX),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Munmap, UnitHint, PageSize), 0u);
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
}

TEST_F(LinuxMemory, MergeAdviceSplitsAndRejoinsWithoutChangingMemory) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(llvm::toString(Space->writeInteger(UnitHint, FirstByte, 8)), "");
  const auto Generation = Space->mappingGeneration();
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize - 1, 12), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint + 3 * PageSize - 1));
  EXPECT_FALSE(Memory->isMergeable(UnitHint - 1));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + 3 * PageSize));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + PageSize, 1, 13), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_TRUE(Memory->isMergeable(UnitHint + 2 * PageSize));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + PageSize, PageSize, 12), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 12), 0u);
  EXPECT_EQ(Space->mappingGeneration(), Generation);
  EXPECT_EQ(RAM->allocatedBytes(), 3 * PageSize);
  EXPECT_EQ(*Space->readInteger(UnitHint, 8), FirstByte);
  EXPECT_TRUE(*Space->canAccess(UnitHint, 3 * PageSize, Read | Write));
}

TEST_F(LinuxMemory, MergeAdviceVisitsBothSidesOfHolesAndIgnoresProtection) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  ASSERT_EQ(call(ServiceKind::Mprotect, UnitHint, PageSize, 0), 0u);
  ASSERT_EQ(
      call(ServiceKind::Mprotect, UnitHint + 2 * PageSize, PageSize, ProtRead),
      0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint - PageSize, 5 * PageSize, 12),
            uint64_t(0) - NoMemory);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_TRUE(Memory->isMergeable(UnitHint + 2 * PageSize));
  EXPECT_FALSE(*Space->canAccess(UnitHint, 1, Read));
  EXPECT_FALSE(*Space->canAccess(UnitHint + 2 * PageSize, 1, Write));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 13),
            uint64_t(0) - NoMemory);
  EXPECT_FALSE(Memory->isMergeable(UnitHint));
  EXPECT_FALSE(Memory->isMergeable(UnitHint + 2 * PageSize));
}

TEST_F(LinuxMemory, UnmapAndBreakShrinkRetireAdviceWithoutPinningPages) {
  ASSERT_EQ(map(3 * PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHint, 3 * PageSize, 12), 0u);
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint + PageSize, PageSize), 0u);
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_EQ(RAM->allocatedBytes(), 2 * PageSize);
  ASSERT_EQ(map(PageSize, UnitHint + PageSize), UnitHint + PageSize);
  EXPECT_FALSE(Memory->isMergeable(UnitHint + PageSize));
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  ASSERT_EQ(call(ServiceKind::Munmap, UnitHint, 3 * PageSize), 0u);
  EXPECT_FALSE(Memory->isMergeable(UnitHint));
  EXPECT_EQ(RAM->allocatedBytes(), 0u);
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * PageSize),
            UnitHeap + 2 * PageSize);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHeap, 2 * PageSize, 12), 0u);
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 1), UnitHeap + 1);
  EXPECT_TRUE(Memory->isMergeable(UnitHeap));
  EXPECT_FALSE(Memory->isMergeable(UnitHeap + PageSize));
  ASSERT_EQ(call(ServiceKind::Brk, UnitHeap + 2 * PageSize),
            UnitHeap + 2 * PageSize);
  EXPECT_FALSE(Memory->isMergeable(UnitHeap + PageSize));
}

TEST_F(LinuxMemory, MergeAdviceValidatesRangesAndConsumesOnlyTheLowInt) {
  ASSERT_EQ(map(PageSize), UnitHint);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, 1, (1ULL << 32) | 12), 0u);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint + 1, 0, 13),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Madvise, UnitHint, UINT64_MAX, 13),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Madvise, uint64_t(0) - PageSize, PageSize, 13),
            uint64_t(0) - InvalidArgument);
  EXPECT_EQ(call(ServiceKind::Madvise, uint64_t(0) - PageSize, 0, 13), 0u);
  EXPECT_EQ(call(ServiceKind::Madvise, UserLimit, PageSize, 13),
            uint64_t(0) - NoMemory);
  EXPECT_TRUE(Memory->isMergeable(UnitHint));
}

TEST_F(LinuxMemory, UnsupportedAdviceCannotInventReturnsOrChangePolicy) {
  ASSERT_EQ(map(PageSize), UnitHint);
  ASSERT_EQ(call(ServiceKind::Madvise, UnitHint, PageSize, 12), 0u);
  ASSERT_EQ(llvm::toString(Space->writeInteger(UnitHint, FirstByte, 8)), "");
  const auto Generation = Space->mappingGeneration();
  // In particular DONTNEED must not be accepted as a content-preserving hint.
  for (uint64_t Advice : {0u, 4u, 8u, 0xffffffffu}) {
    EXPECT_FALSE(call(ServiceKind::Madvise, UnitHint, PageSize, Advice));
    EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Report.Diagnostic.find("madvise"), std::string::npos);
    EXPECT_TRUE(Memory->isMergeable(UnitHint));
    EXPECT_EQ(*Space->readInteger(UnitHint, 8), FirstByte);
  }
  EXPECT_EQ(Space->mappingGeneration(), Generation);
}
} // namespace
} // namespace neverd::emulation
