//===- DarwinPselectTests.cpp - Literal immediate select contracts
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bitmap, ordering and permission contracts for pselect.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinPselectTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page, Time;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinSystemOptions> System;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only pselect"};
  void SetUp() override {
    Page = GetParam();
    Time = Base + Page * 4 + 16;
    auto Physical = PhysicalMemory::create(Page * 6);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 6);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 6, Read | Write | UserAccessible)));
    Options.emplace().Files["/data"] = {'a', 'b', 'c'};
    Options->WritableFiles.insert("/data");
    Options->SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
    auto &M = Options->Metadata["/data"];
    M.Mode = 0100600;
    M.Inode = 42;
    M.LinkCount = 1;
    M.Size = 3;
    M.BlockSize = 512;
    M.Blocks = 1;
    M.AccessTime = {7, 8};
    M.ModificationTime = {9, 10};
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    timer();
  }
  void timer(uint64_t Seconds = 0, uint64_t Nanoseconds = 0) {
    std::array<uint8_t, 16> B;
    llvm::support::endian::write64le(B.data(), Seconds);
    llvm::support::endian::write64le(B.data() + 8, Nanoseconds);
    llvm::cantFail(Space->write(Time, B));
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result =
        ProcessResult{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                      ExecutionBackendKind::Unicorn, "memory-only pselect"};
    auto Out = Files->handle(K, {0, 0, A, {}}, Result, System);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  void returned(std::array<uint64_t, 6> A, uint64_t Value, bool Error = false) {
    auto O = invoke(ServiceKind::Pselect, A);
    ASSERT_TRUE(O) << Result.Diagnostic;
    EXPECT_EQ(O->Value, Value);
    EXPECT_EQ(O->Error, Error);
  }
  void refused(std::array<uint64_t, 6> A, const char *Reason) {
    EXPECT_FALSE(invoke(ServiceKind::Pselect, A));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Reason);
  }
  uint32_t open(uint32_t Flags = 0, llvm::StringRef Path = "/data") {
    const auto Text = Path.str() + '\0';
    llvm::cantFail(Space->write(
        Base + Page * 5,
        llvm::ArrayRef<uint8_t>((const uint8_t *)Text.data(), Text.size())));
    auto O = invoke(ServiceKind::Open, {Base + Page * 5, Flags});
    EXPECT_TRUE(O) << Result.Diagnostic;
    EXPECT_FALSE(O && O->Error);
    return O ? uint32_t(O->Value) : UINT32_MAX;
  }
  uint32_t dupAt(uint32_t FD, uint32_t Minimum) {
    auto O = invoke(ServiceKind::Fcntl, {FD, 0, Minimum});
    EXPECT_TRUE(O) << Result.Diagnostic;
    EXPECT_FALSE(O && O->Error);
    return O ? uint32_t(O->Value) : UINT32_MAX;
  }
  std::array<uint8_t, 32> snapshot(uint64_t Address) {
    std::array<uint8_t, 32> B;
    llvm::cantFail(Space->read(Address - 8, B));
    return B;
  }
  std::array<uint8_t, 32> bitmap(uint64_t Address,
                                 std::initializer_list<uint32_t> Words) {
    std::array<uint8_t, 32> B;
    B.fill(0xa5);
    size_t I = 8;
    for (uint32_t W : Words) {
      llvm::support::endian::write32le(B.data() + I, W);
      I += 4;
    }
    llvm::cantFail(Space->write(Address - 8, B));
    return B;
  }
  void expectBitmap(uint64_t Address, std::array<uint8_t, 32> Before,
                    std::initializer_list<uint32_t> Words) {
    size_t I = 8;
    for (uint32_t W : Words) {
      llvm::support::endian::write32le(Before.data() + I, W);
      I += 4;
    }
    EXPECT_EQ(snapshot(Address), Before);
  }
};
TEST_P(DarwinPselectTest, TimespecImportAndValidationPrecedeMaskAndCount) {
  returned({UINT32_MAX, 0, 0, 0, UINT64_MAX, UINT64_MAX}, 14, true);
  const auto Short = Base + Page * 6 - 8;
  llvm::cantFail(Space->writeInteger(Short, 0, 8));
  returned({UINT32_MAX, 0, 0, 0, Short, UINT64_MAX}, 14, true);
  for (uint64_t Seconds :
       std::array<uint64_t, 3>{UINT64_MAX, 2147483648ULL, 0x100000000ULL}) {
    timer(Seconds);
    returned({UINT32_MAX, 0, 0, 0, Time, UINT64_MAX}, 22, true);
  }
  for (uint64_t Nsec :
       std::array<uint64_t, 3>{UINT64_MAX, 1000000001ULL, 0x100000000ULL}) {
    timer(0, Nsec);
    returned({UINT32_MAX, 0, 0, 0, Time, UINT64_MAX}, 22, true);
  }
  for (const auto &[Seconds, Nsec] :
       {std::pair{2147483647ULL, 1000000000ULL}, std::pair{0ULL, 1000000000ULL},
        std::pair{1ULL, 0ULL}}) {
    timer(Seconds, Nsec);
    returned({UINT32_MAX, 0, 0, 0, Time, 0}, 22, true);
    returned({UINT32_MAX, 0, 0, 0, Time, UINT64_MAX}, 14, true);
    refused({0, UINT64_MAX, UINT64_MAX, UINT64_MAX, Time, 0},
            diagnostic::PselectWait);
  }
  timer();
  auto Before = snapshot(Time);
  llvm::cantFail(Space->protect(Base + Page * 4, Page, Read | UserAccessible));
  returned({0, UINT64_MAX, UINT64_MAX, UINT64_MAX, Time, 0}, 0);
  EXPECT_EQ(snapshot(Time), Before);
}
TEST_P(DarwinPselectTest,
       MaskImportPrecedesCountAndCompleteMasksStayUnsupported) {
  timer();
  const auto Mask = Base + Page * 4 + 64;
  const auto Short = Base + Page * 6 - 2;
  llvm::cantFail(Space->writeInteger(Short, 0, 2));
  returned({UINT32_MAX, 0, 0, 0, Time, UINT64_MAX}, 14, true);
  returned({UINT32_MAX, 0, 0, 0, Time, Short}, 14, true);
  returned({UINT32_MAX, 0, 0, 0, 0, UINT64_MAX}, 14, true);
  const auto Set = Base + Page * 2 + 64;
  auto Before = bitmap(Set, {0x80000000});
  for (uint32_t Bits : {0u, UINT32_MAX}) {
    llvm::cantFail(Space->writeInteger(Mask, Bits, 4));
    const auto MaskBefore = snapshot(Mask);
    refused({UINT32_MAX, Set, Set, Set, Time, Mask}, diagnostic::PselectMask);
    refused({0, Set, Set, Set, Time, Mask}, diagnostic::PselectMask);
    EXPECT_EQ(snapshot(Set), Before);
    EXPECT_EQ(snapshot(Mask), MaskBefore);
  }
  llvm::cantFail(Space->protect(Base + Page * 4, Page, Read | UserAccessible));
  refused({0, Set, Set, Set, Time, Mask}, diagnostic::PselectMask);
  EXPECT_EQ(snapshot(Set), Before);
}
TEST_P(DarwinPselectTest, NullMaskRetainsFullPointersAndSignedLowCount) {
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    returned({High, UINT64_MAX, UINT64_MAX, UINT64_MAX, Time, 0}, 0);
    returned({High | UINT32_MAX, UINT64_MAX, 0, 0, Time, 0}, 22, true);
    returned({High | 0x80000000u, 0, 0, 0, Time, 0}, 22, true);
    returned({High | UINT32_MAX, 0, 0, 0, 0, 0}, 22, true);
    refused({High, 0, 0, 0, 0, 0}, diagnostic::PselectWait);
  }
  const auto FD = open();
  ASSERT_EQ(FD, 3u);
  const uint64_t Set = Base + Page * 2 + 64;
  auto B = bitmap(Set, {0x80000008});
  returned({0x1234567800000004ULL, Set, 0, 0, Time, 0}, 1);
  expectBitmap(Set, B, {8});
  returned({4, Set | 0x100000000ULL, 0, 0, Time, 0}, 14, true);
  returned({4, 0, 0, 0, Time | 0x100000000ULL, 0}, 14, true);
}
TEST_P(DarwinPselectTest, LiveKeysProveOnlyTheAdmittedClampLowerBound) {
  refused({4, UINT64_MAX, 0, 0, Time}, diagnostic::SelectCapacity);
  const auto FD = open();
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const uint64_t Set = Base + Page * 2 + 64;
  System.reset();
  auto B = bitmap(Set, {8, 0, 0xffffffff});
  returned({65, Set, 0, 0, Time}, 2);
  expectBitmap(Set, B, {8, 0, 1});
  System.emplace().MaxFilesPerProcess = 1;
  System->ResourceLimits[8] = {2, 10240};
  B = bitmap(Set, {8, 0, 0xffffffff});
  returned({65, Set, 0, 0, Time}, 2);
  expectBitmap(Set, B, {8, 0, 1});
  refused({66, UINT64_MAX, 0, 0, Time}, diagnostic::SelectCapacity);
  ASSERT_TRUE(invoke(ServiceKind::Close, {64}));
  refused({65, UINT64_MAX, 0, 0, Time}, diagnostic::SelectCapacity);
  returned({4, 0, 0, 0, Time}, 0);
  EXPECT_EQ(System->ResourceLimits.at(8).Current, 2u);
  EXPECT_EQ(Options->DescriptorLimit, 256u);
  for (uint32_t FD : {0u, 1u, 2u, 3u}) {
    auto O = invoke(ServiceKind::Close, {FD});
    ASSERT_TRUE(O);
    EXPECT_FALSE(O->Error);
  }
  refused({1, UINT64_MAX, UINT64_MAX, UINT64_MAX, Time},
          diagnostic::SelectCapacity);
  returned({0, UINT64_MAX, UINT64_MAX, UINT64_MAX, Time}, 0);
}
TEST_P(DarwinPselectTest, BitmapRoundingClearsOnlySelectedWordsAndTailBits) {
  const auto FD = open();
  ASSERT_EQ(FD, 3u);
  ASSERT_EQ(dupAt(FD, 31), 31u);
  ASSERT_EQ(dupAt(FD, 32), 32u);
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const uint64_t Set = Base + Page * 2 + 64;
  auto B = bitmap(Set, {0x80000008, 0x80000001, 0xffffffff});
  returned({31, Set, 0, 0, Time}, 1);
  expectBitmap(Set, B, {8});
  B = bitmap(Set, {0x80000008, 0x80000001, 0xffffffff});
  returned({32, Set, 0, 0, Time}, 2);
  expectBitmap(Set, B, {0x80000008});
  B = bitmap(Set, {0x80000008, 0x80000001, 0xffffffff});
  returned({33, Set, 0, 0, Time}, 3);
  expectBitmap(Set, B, {0x80000008, 1});
  B = bitmap(Set, {0x80000008, 1, 0xffffffff});
  returned({64, Set, 0, 0, Time}, 3);
  expectBitmap(Set, B, {0x80000008, 1});
  B = bitmap(Set, {0x80000008, 1, 0xffffffff});
  returned({65, Set, 0, 0, Time}, 4);
  expectBitmap(Set, B, {0x80000008, 1, 1});
}
TEST_P(DarwinPselectTest, ThreeClassesCountBitsAndKeepDescriptionState) {
  const auto FD = open();
  ASSERT_EQ(FD, 3u);
  auto Dup = invoke(ServiceKind::Dup, {FD});
  ASSERT_TRUE(Dup);
  ASSERT_EQ(Dup->Value, 4u);
  auto Seek = invoke(ServiceKind::Lseek, {FD, 3, 0});
  ASSERT_TRUE(Seek);
  ASSERT_EQ(Seek->Value, 3u);
  ASSERT_TRUE(invoke(ServiceKind::Fcntl, {FD, 4, 4}));
  const auto Stat = [&] {
    auto O = invoke(ServiceKind::Fstat64, {FD, Base + Page * 3 + 64});
    EXPECT_TRUE(O);
    EXPECT_FALSE(O && O->Error);
    std::array<uint8_t, 144> B;
    llvm::cantFail(Space->read(Base + Page * 3 + 64, B));
    return B;
  };
  auto Original = Stat();
  const std::array<uint64_t, 3> Address{
      Base + Page * 2 + 64, Base + Page * 2 + 128, Base + Page * 2 + 192};
  std::array<std::array<uint8_t, 32>, 3> B;
  for (size_t I = 0; I != 3; ++I)
    B[I] = bitmap(Address[I], {0x80000018});
  returned({5, Address[0], Address[1], Address[2], Time}, 6);
  for (size_t I = 0; I != 3; ++I)
    expectBitmap(Address[I], B[I], {24});
  EXPECT_EQ(Stat(), Original);
  Seek = invoke(ServiceKind::Lseek, {4, 0, 1});
  ASSERT_TRUE(Seek);
  EXPECT_EQ(Seek->Value, 3u);
  auto Flags = invoke(ServiceKind::Fcntl, {4, 3});
  ASSERT_TRUE(Flags);
  EXPECT_EQ(Flags->Value, 4u);
  EXPECT_EQ(Options->Files.at("/data"), (std::vector<uint8_t>{'a', 'b', 'c'}));
}
TEST_P(DarwinPselectTest, AllCopyinsAndAllDescriptorsPrecedeEveryProvider) {
  const auto FD = open();
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const uint64_t Set = Base + Page * 2 + 64;
  auto B = bitmap(Set, {0x20, 0, 0});
  returned({65, Set, UINT64_MAX, 0, Time}, 14, true);
  EXPECT_EQ(snapshot(Set), B);
  auto X = bitmap(Set + 64, {0x20, 0, 0});
  B = bitmap(Set, {1, 0, 0});
  returned({65, Set, 0, Set + 64, Time}, 9, true);
  EXPECT_EQ(snapshot(Set), B);
  EXPECT_EQ(snapshot(Set + 64), X);
  for (uint32_t Unknown :
       {0u, 1u, 2u, open(0, "/"), open(0x200000, "/alias")}) {
    B = bitmap(Set, {uint32_t{1} << Unknown, 0, 0});
    refused({65, Set, 0, 0, Time}, diagnostic::SelectObject);
    EXPECT_EQ(snapshot(Set), B);
    returned({65, 0, 0, 0, Time}, 0);
  }
}
TEST_P(DarwinPselectTest, WholeInputFaultAndClosedBitsPublishNoOutputs) {
  const auto FD = open();
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const uint64_t Set = Base + Page * 2 + 64;
  auto B = bitmap(Set, {0x20, 0, 0});
  const uint64_t Tail = Base + Page * 6 - 8;
  llvm::cantFail(Space->writeInteger(Tail, 0, 8));
  returned({65, Set, Tail, 0, Time}, 14, true);
  EXPECT_EQ(snapshot(Set), B);
  returned({65, Set, 0, 0, Time}, 9, true);
  EXPECT_EQ(snapshot(Set), B);
}
TEST_P(DarwinPselectTest, WholeReadonlySetsStillAllowLaterOrderedCopyouts) {
  const auto FD = open();
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const std::array<uint64_t, 3> Address{Base + 64, Base + Page + 64,
                                        Base + Page * 2 + 64};
  for (unsigned Denied = 0; Denied != 3; ++Denied) {
    std::array<std::array<uint8_t, 32>, 3> B;
    for (unsigned I = 0; I != 3; ++I)
      B[I] = bitmap(Address[I], {8, 0x80000000});
    llvm::cantFail(
        Space->protect(Base + Page * Denied, Page, Read | UserAccessible));
    returned({33, Address[0], Address[1], Address[2], Time}, 14, true);
    for (unsigned I = 0; I != 3; ++I)
      if (I == Denied)
        EXPECT_EQ(snapshot(Address[I]), B[I]);
      else
        expectBitmap(Address[I], B[I], {8, 0});
    llvm::cantFail(Space->protect(Base + Page * Denied, Page,
                                  Read | Write | UserAccessible));
  }
}
TEST_P(DarwinPselectTest, PartialOutputsInEitherDirectionRefuseEveryCopyout) {
  const auto FD = open();
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const uint64_t Span = Base + Page - 4;
  for (unsigned Position = 0; Position != 3; ++Position) {
    for (unsigned ReadonlyPage : {0u, 1u}) {
      std::array<uint64_t, 3> Address{
          Base + Page * 2 + 64, Base + Page * 2 + 128, Base + Page * 2 + 192};
      Address[Position] = Span;
      std::array<std::array<uint8_t, 32>, 3> B;
      for (unsigned I = 0; I != 3; ++I)
        B[I] = bitmap(Address[I], {8, 0x80000000});
      llvm::cantFail(Space->protect(Base + Page * ReadonlyPage, Page,
                                    Read | UserAccessible));
      refused({33, Address[0], Address[1], Address[2], Time},
              diagnostic::SelectPartialOutput);
      for (unsigned I = 0; I != 3; ++I)
        EXPECT_EQ(snapshot(Address[I]), B[I]);
      llvm::cantFail(Space->protect(Base + Page * ReadonlyPage, Page,
                                    Read | Write | UserAccessible));
    }
  }
}
TEST_P(DarwinPselectTest, OverlapsRefuseAndAdjacentRangesRemainDistinct) {
  const auto FD = open();
  ASSERT_EQ(dupAt(FD, 64), 64u);
  const uint64_t Set = Base + Page * 2 + 64;
  auto B = bitmap(Set, {8, 0, 8});
  refused({33, Set, Set, 0, Time}, diagnostic::SelectOverlap);
  EXPECT_EQ(snapshot(Set), B);
  refused({33, Set, Set + 4, 0, Time}, diagnostic::SelectOverlap);
  EXPECT_EQ(snapshot(Set), B);
  // Zero nsec bits still import a readable bitmap that overlaps the timespec.
  timer();
  auto T = snapshot(Time);
  refused({4, Time + 8, 0, 0, Time}, diagnostic::SelectOverlap);
  EXPECT_EQ(snapshot(Time), T);
  llvm::cantFail(Space->writeInteger(Set, 8, 4));
  llvm::cantFail(Space->writeInteger(Set + 4, 0x80000000, 4));
  llvm::cantFail(Space->writeInteger(Set + 8, 8, 4));
  llvm::cantFail(Space->writeInteger(Set + 12, 0x80000000, 4));
  returned({33, Set, Set + 8, 0, Time}, 2);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Set, 8)), 8u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Set + 8, 8)), 8u);
  llvm::cantFail(Space->writeInteger(Time + 16, 0x80000000, 4));
  returned({4, Time + 16, 0, 0, Time}, 0);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Time + 16, 4)), 0u);
  returned({0, Time, Time, Time, Time}, 0);
}
TEST_P(DarwinPselectTest, CloseReuseAndDup2UseCurrentNumericDescriptions) {
  const auto FD = open();
  auto Dup = invoke(ServiceKind::Dup, {FD});
  ASSERT_TRUE(Dup);
  ASSERT_EQ(Dup->Value, 4u);
  ASSERT_TRUE(invoke(ServiceKind::Close, {FD}));
  const uint64_t Set = Base + Page * 2 + 64;
  auto B = bitmap(Set, {8});
  returned({5, Set, 0, 0, Time}, 9, true);
  EXPECT_EQ(snapshot(Set), B);
  ASSERT_EQ(open(1), FD);
  B = bitmap(Set, {0x80000008});
  returned({5, Set, 0, 0, Time}, 1);
  expectBitmap(Set, B, {8});
  ASSERT_TRUE(invoke(ServiceKind::Dup2, {1, FD}));
  B = bitmap(Set, {8});
  refused({5, Set, 0, 0, Time}, diagnostic::SelectObject);
  EXPECT_EQ(snapshot(Set), B);
  B = bitmap(Set, {16});
  returned({5, Set, 0, 0, Time}, 1);
  expectBitmap(Set, B, {16});
}
INSTANTIATE_TEST_SUITE_P(OSPages, DarwinPselectTest,
                         testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
