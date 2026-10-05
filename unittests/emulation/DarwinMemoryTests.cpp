//===- DarwinMemoryTests.cpp - Darwin VM errors and ownership ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinMemoryTest : public testing::TestWithParam<uint64_t> {
protected:
  std::shared_ptr<AddressSpace> Space;
  std::unique_ptr<DarwinMemory> Memory;
  ProcessOptions Options;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "test"};
  uint64_t Page = 0;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    Options.MemoryLimit = Page * 4;
    Options.StackSize = Page;
    MemoryLayout Layout{Page, 0x100000000ULL, {}};
    Memory = std::make_unique<DarwinMemory>(*Space, Layout, Options);
    Files = std::make_unique<DarwinFiles>(*Space, Options.DarwinFiles);
  }
  ServiceResult call(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    ProcessServiceEvent E{0, 0, Args, std::nullopt};
    auto Value = Memory->handle(Kind, E, *Files, Result);
    EXPECT_TRUE(bool(Value))
        << (Value ? "" : llvm::toString(Value.takeError()));
    if (!Value || !*Value)
      return {UINT64_MAX, true};
    return **Value;
  }
  uint64_t allocate(uint64_t Size) {
    auto Value = call(ServiceKind::Mmap, {0, Size, 3, 0x1002, UINT64_MAX, 0});
    EXPECT_FALSE(Value.Error);
    EXPECT_EQ(Value.Value % Page, 0u);
    return Value.Value;
  }
  ServiceResult fileCall(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    auto Value = Files->handle(Kind, {0, 0, Args, std::nullopt}, Result);
    EXPECT_TRUE(bool(Value))
        << (Value ? "" : llvm::toString(Value.takeError()));
    if (!Value || !*Value)
      return {UINT64_MAX, true};
    return **Value;
  }
  uint64_t openFile(std::vector<uint8_t> Bytes) {
    if (!Options.DarwinFiles)
      Options.DarwinFiles.emplace();
    Options.DarwinFiles->Files["/data"] = std::move(Bytes);
    const uint64_t Address = 0x100000;
    llvm::cantFail(Space->map(Address, Page, Read | Write | UserAccessible));
    const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
    llvm::cantFail(Space->write(Address, Path));
    auto FD = fileCall(ServiceKind::Open, {Address, 0});
    EXPECT_FALSE(FD.Error);
    llvm::cantFail(Space->unmap(Address, Page));
    return FD.Value;
  }
};
TEST_P(DarwinMemoryTest, PartialUnmapRetiresBudgetAndFreshMappingIsZero) {
  const auto Address = allocate(Page * 4);
  ASSERT_FALSE(bool(Space->writeInteger(Address + Page, 0xff, 1)));
  auto Full = call(ServiceKind::Mmap, {0, Page, 3, 0x1002, UINT64_MAX, 0});
  EXPECT_TRUE(Full.Error);
  EXPECT_EQ(Full.Value, 12u);
  EXPECT_EQ(call(ServiceKind::Munmap, {Address + Page, Page, 0, 0, 0, 0}).Value,
            0u);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page * 3);
  auto Reused =
      call(ServiceKind::Mmap, {Address + Page, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(Reused.Error);
  EXPECT_EQ(Reused.Value, Address + Page);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Reused.Value, 1)), 0u);
}
TEST_P(DarwinMemoryTest, ProtectionAcrossHoleCannotChangeEarlierPages) {
  auto Address = allocate(Page * 3);
  EXPECT_FALSE(
      call(ServiceKind::Munmap, {Address + Page, Page, 0, 0, 0, 0}).Error);
  auto Failed = call(ServiceKind::Mprotect, {Address, Page * 3, 0, 0, 0, 0});
  EXPECT_TRUE(Failed.Error);
  EXPECT_EQ(Failed.Value, 12u);
  EXPECT_TRUE(
      llvm::cantFail(Space->canAccess(Address, Page, Write | UserAccessible)));
  EXPECT_TRUE(llvm::cantFail(
      Space->canAccess(Address + Page * 2, Page, Write | UserAccessible)));
}
TEST_P(DarwinMemoryTest, AlignmentNoneAndWriteOnlyUseDarwinPageRules) {
  const auto Address = allocate(Page);
  auto Bad = call(ServiceKind::Mprotect, {Address + 1, Page, 1, 0, 0, 0});
  EXPECT_TRUE(Bad.Error);
  EXPECT_EQ(Bad.Value, 22u);
  if (Page == 16384) {
    Bad = call(ServiceKind::Munmap, {Address + 4096, 4096, 0, 0, 0, 0});
    EXPECT_TRUE(Bad.Error);
    EXPECT_EQ(Bad.Value, 22u);
  }
  EXPECT_FALSE(call(ServiceKind::Mprotect, {Address, Page, 0, 0, 0, 0}).Error);
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(Address, 1, Read | UserAccessible)));
  EXPECT_FALSE(call(ServiceKind::Mprotect, {Address, Page, 2, 0, 0, 0}).Error);
  EXPECT_TRUE(llvm::cantFail(
      Space->canAccess(Address, Page, Read | Write | UserAccessible)));
}
TEST_P(DarwinMemoryTest, MaximumProtectionFailureIsAtomicAcrossSegments) {
  const uint64_t Address = 0x100000000ULL;
  ASSERT_FALSE(bool(Space->map(Address, Page * 2, Read | UserAccessible)));
  ProcessOptions Options;
  Options.MemoryLimit = Page * 4;
  Options.StackSize = Page;
  MemoryLayout Layout{
      Page, Address, {{Address, Page, 3}, {Address + Page, Page, 1}}};
  Memory = std::make_unique<DarwinMemory>(*Space, Layout, Options);
  auto Denied = call(ServiceKind::Mprotect, {Address, Page * 2, 3, 0, 0, 0});
  EXPECT_TRUE(Denied.Error);
  EXPECT_EQ(Denied.Value, 13u);
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(Address, 1, Write | UserAccessible)));
  EXPECT_TRUE(llvm::cantFail(
      Space->canAccess(Address, Page * 2, Read | UserAccessible)));
}
TEST_P(DarwinMemoryTest, UnsupportedModesHaveNoMappingEffects) {
  for (auto Args : {std::array<uint64_t, 6>{0, Page, 7, 0x1002, UINT64_MAX, 0},
                    std::array<uint64_t, 6>{0, Page, 3, 0x1012, UINT64_MAX, 0},
                    std::array<uint64_t, 6>{0, Page, 3, 2, 0, 0}}) {
    ProcessServiceEvent E{0, 197, Args, std::nullopt};
    auto Value = Memory->handle(ServiceKind::Mmap, E, *Files, Result);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    EXPECT_FALSE(*Value);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Space->mappedBytes(), 0u);
    EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
  }
}
TEST_P(DarwinMemoryTest, RawMmapZeroLengthAndInvalidUnmapRemainDistinct) {
  const auto Before = Space->mappedBytes();
  for (auto Kind : {ServiceKind::Mmap, ServiceKind::Munmap}) {
    auto Zero = call(Kind, {0, 0, 3, 0x1002, UINT64_MAX, 0});
    EXPECT_EQ(Zero.Error, Kind == ServiceKind::Munmap);
    EXPECT_EQ(Zero.Value, Kind == ServiceKind::Munmap ? 22u : 0u);
    auto Overflow = call(Kind, {0, UINT64_MAX, 3, 0x1002, UINT64_MAX, 0});
    EXPECT_TRUE(Overflow.Error);
    EXPECT_EQ(Overflow.Value, 22u);
  }
  EXPECT_EQ(Space->mappedBytes(), Before);
}
TEST_P(DarwinMemoryTest, NonfixedHintRoundsUpAndSearchesAboveOccupiedHint) {
  const uint64_t Hint = 0x2000000000ULL;
  auto First =
      call(ServiceKind::Mmap, {Hint + 1, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(First.Error);
  EXPECT_EQ(First.Value, Hint + Page);
  auto Second =
      call(ServiceKind::Mmap, {Hint + 1, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(Second.Error);
  EXPECT_EQ(Second.Value, Hint + Page * 2);
  auto Fallback = call(ServiceKind::Mmap,
                       {value::UserLimit - 1, Page, 3, 0x1002, UINT64_MAX, 0});
  ASSERT_FALSE(Fallback.Error);
  EXPECT_LT(Fallback.Value, Hint);
}
TEST_P(DarwinMemoryTest, PrivateFileMappingsCopyPagesWithoutChangingFileState) {
  std::vector<uint8_t> Bytes(Page + 19);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I * 17 + 5);
  const auto FD = openFile(Bytes);
  EXPECT_EQ(fileCall(ServiceKind::Lseek, {FD, 7, 0}).Value, 7u);
  const auto First = call(ServiceKind::Mmap, {0, 1, 3, 2, FD, 0});
  ASSERT_FALSE(First.Error);
  // mmap admits a whole page even when the requested byte count is one.
  EXPECT_EQ(llvm::cantFail(Space->readInteger(First.Value + Page - 1, 1)),
            Bytes[Page - 1]);
  const auto Tail = call(ServiceKind::Mmap, {0, 19, 0, 0x40002, FD, Page});
  ASSERT_FALSE(Tail.Error);
  EXPECT_FALSE(
      llvm::cantFail(Space->canAccess(Tail.Value, 1, Read | UserAccessible)));
  EXPECT_FALSE(call(ServiceKind::Mprotect, {Tail.Value, Page, 2}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value, 1)), Bytes[Page]);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + 18, 1)),
            Bytes.back());
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + 19, 1)), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + Page - 1, 1)), 0u);
  llvm::cantFail(Space->writeInteger(First.Value, 0xaa, 1));
  llvm::cantFail(Space->writeInteger(Tail.Value + Page - 1, 0xbb, 1));
  const auto Second = call(ServiceKind::Mmap, {0, Page, 1, 2, FD, 0});
  ASSERT_FALSE(Second.Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Second.Value, 1)), Bytes[0]);
  EXPECT_EQ(Options.DarwinFiles->Files.at("/data"), Bytes);
  EXPECT_EQ(fileCall(ServiceKind::Lseek, {FD, 0, 1}).Value, 7u);
  EXPECT_FALSE(fileCall(ServiceKind::Close, {FD}).Error);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(First.Value, 1)), 0xaau);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Tail.Value + Page - 1, 1)),
            0xbbu);
  EXPECT_EQ(call(ServiceKind::Mmap, {0, 1, 1, 2, FD, 0}).Value, 9u);
}
TEST_P(DarwinMemoryTest, FileMappingValidationPrecedesDescriptorAndBudget) {
  const auto FD = openFile({1, 2, 3});
  const auto Address = allocate(Page * 4);
  for (auto Args : {std::array<uint64_t, 6>{0, 0, 1, 0x40002, 99, 0},
                    std::array<uint64_t, 6>{0, 1, 1, 0x40002, 99, 1},
                    std::array<uint64_t, 6>{0, UINT64_MAX, 1, 2, 99, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 2, 99, 0 - Page}}) {
    const auto V = call(ServiceKind::Mmap, Args);
    EXPECT_TRUE(V.Error);
    EXPECT_EQ(V.Value, 22u);
  }
  for (uint64_t Length : {uint64_t(0), Page}) {
    auto Bad = call(ServiceKind::Mmap, {0, Length, 1, 2, 99, 0});
    EXPECT_TRUE(Bad.Error);
    EXPECT_EQ(Bad.Value, 9u);
  }
  const auto Zero = call(ServiceKind::Mmap, {0, 0, 1, 2, FD, 0});
  EXPECT_FALSE(Zero.Error);
  EXPECT_EQ(Zero.Value, 0u);
  const auto Full = call(ServiceKind::Mmap, {0, Page, 1, 2, FD, 0});
  EXPECT_TRUE(Full.Error);
  EXPECT_EQ(Full.Value, 12u);
  EXPECT_EQ(Space->mappedBytes(), Page * 4);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Page * 4);
  EXPECT_FALSE(call(ServiceKind::Munmap, {Address, Page}).Error);
  const auto Reused = call(ServiceKind::Mmap, {Address, 3, 1, 2, FD, 0});
  EXPECT_FALSE(Reused.Error);
  EXPECT_EQ(Reused.Value, Address);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Address, 1)), 1u);
}
TEST_P(DarwinMemoryTest, UnknownFileMappingEffectsStopBeforeAllocation) {
  const auto FD = openFile({1, 2, 3});
  for (auto Args : {std::array<uint64_t, 6>{0, 1, 1, 2, FD, 1},
                    std::array<uint64_t, 6>{0, Page + 1, 1, 2, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 2, FD, Page},
                    std::array<uint64_t, 6>{0, 1, 1, 2, FD, uint64_t(1) << 63},
                    std::array<uint64_t, 6>{0, Page, 1, 1, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 5, 2, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 0x12, FD, 0},
                    std::array<uint64_t, 6>{0, Page, 1, 2, 1, 0}}) {
    auto V = Memory->handle(ServiceKind::Mmap, {0, 197, Args, std::nullopt},
                            *Files, Result);
    ASSERT_TRUE(bool(V)) << llvm::toString(V.takeError());
    EXPECT_FALSE(*V);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_FALSE(Result.Diagnostic.empty());
    EXPECT_EQ(Space->mappedBytes(), 0u);
    EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), 0u);
    EXPECT_EQ(fileCall(ServiceKind::Lseek, {FD, 0, 1}).Value, 0u);
  }
}
TEST_P(DarwinMemoryTest, EmptyFilesHaveNoMappablePageButAllowLegacyZeroLength) {
  const auto FD = openFile({});
  const auto Zero = call(ServiceKind::Mmap, {0, 0, 1, 2, FD, 0});
  EXPECT_FALSE(Zero.Error);
  EXPECT_EQ(Zero.Value, 0u);
  auto V = Memory->handle(ServiceKind::Mmap,
                          {0, 197, {0, 1, 1, 2, FD, 0}, std::nullopt}, *Files,
                          Result);
  ASSERT_TRUE(bool(V)) << llvm::toString(V.takeError());
  EXPECT_FALSE(*V);
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Space->mappedBytes(), 0u);
}
INSTANTIATE_TEST_SUITE_P(Pages, DarwinMemoryTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
