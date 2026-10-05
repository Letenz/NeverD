//===- DarwinTimeTests.cpp - Darwin clock values and copy phases ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinTimeTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinTime.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinTimeTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinTimeOptions> Options = darwin_test::timeOptions();
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  uint64_t Page;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    fill();
  }
  void fill() {
    ASSERT_FALSE(
        bool(Space->write(Base, std::vector<uint8_t>(Page * 2, 0xa5))));
  }
  std::string bytes(uint64_t Address, size_t Count) {
    std::vector<uint8_t> Data(Count);
    auto E = Space->read(Address, Data);
    EXPECT_FALSE(bool(E));
    llvm::consumeError(std::move(E));
    return std::string(Data.begin(), Data.end());
  }
  std::optional<ServiceResult> invoke(uint64_t TV, uint64_t TZ,
                                      uint64_t Ticks) {
    auto Out = timeService(*Space, {0, 116, {TV, TZ, Ticks}, std::nullopt},
                           Options, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  void ok(uint64_t TV, uint64_t TZ, uint64_t Ticks) {
    auto Out = invoke(TV, TZ, Ticks);
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_FALSE(Out->Error);
    EXPECT_EQ(Out->Value, 0u);
  }
  void fault(uint64_t TV, uint64_t TZ, uint64_t Ticks) {
    auto Out = invoke(TV, TZ, Ticks);
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_TRUE(Out->Error);
    EXPECT_EQ(Out->Value, 14u);
  }
};

TEST_P(DarwinTimeTest, UnalignedLayoutPaddingAndCanariesMatchIndependentBytes) {
  ok(Base + 1, Base + 17, Base + 25);
  EXPECT_EQ(bytes(Base + 1, 32), llvm::fromHex(darwin_test::TimeHex));
  EXPECT_EQ(bytes(Base, 1), std::string(1, '\xa5'));
  EXPECT_EQ(bytes(Base + 33, 7), std::string(7, '\xa5'));
  // Repeating a query does not advance any fixed observation.
  ok(Base + 65, Base + 81, Base + 89);
  EXPECT_EQ(bytes(Base + 65, 32), bytes(Base + 1, 32));
  // A 4 KiB boundary inside a 16 KiB guest page uses the same copy contract.
  ok(Base + 4093, 0, 0);
  EXPECT_EQ(bytes(Base + 4093, 16), bytes(Base + 1, 16));
}

TEST_P(DarwinTimeTest, IntegerExtremesAndExplicitZeroAreLossless) {
  Options = DarwinTimeOptions{DarwinTimeOfDay{UINT32_MAX, 999999},
                              DarwinTimezone{INT32_MIN, INT32_MAX}, UINT64_MAX};
  ok(Base, Base + 16, Base + 24);
  EXPECT_EQ(
      bytes(Base, 32),
      llvm::fromHex(
          "ffffffff000000003f420f000000000000000080ffffff7fffffffffffffffff"));
  Options = DarwinTimeOptions{DarwinTimeOfDay{}, DarwinTimezone{}, 0};
  ok(Base, Base + 16, Base + 24);
  EXPECT_EQ(bytes(Base, 32), std::string(32, '\0'));
}

TEST_P(DarwinTimeTest, AllNullAndSelectiveOutputsRequireOnlyObservedValues) {
  Options.reset();
  ok(0, 0, 0);
  EXPECT_EQ(bytes(Base, 32), std::string(32, '\xa5'));
  Options.emplace();
  ok(0, 0, 0);
  Options->Timezone = DarwinTimezone{-480, -1};
  ok(0, Base, 0);
  EXPECT_EQ(bytes(Base, 8), llvm::fromHex("20feffffffffffff"));
  Options->Timezone.reset();
  Options->MachAbsoluteTime = 0;
  ok(0, 0, Base);
  EXPECT_EQ(bytes(Base, 8), std::string(8, '\0'));
  Options->MachAbsoluteTime.reset();
  Options->TimeOfDay = DarwinTimeOfDay{};
  ok(Base, 0, 0);
  EXPECT_EQ(bytes(Base, 16), std::string(16, '\0'));
}

TEST_P(DarwinTimeTest, MissingJointSamplePrecedesPointersAndAllCopies) {
  Options->MachAbsoluteTime.reset();
  for (auto TV : {Base, uint64_t(1)}) {
    EXPECT_FALSE(invoke(TV, Base + 16, Base + 24));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::TimeAbsoluteMissing);
    EXPECT_EQ(bytes(Base, 32), std::string(32, '\xa5'));
  }
  Options = darwin_test::timeOptions();
  Options->TimeOfDay.reset();
  EXPECT_FALSE(invoke(Base, Base + 16, Base + 24));
  EXPECT_EQ(Result.Diagnostic, diagnostic::TimeOfDayMissing);
  EXPECT_EQ(bytes(Base, 32), std::string(32, '\xa5'));
  Options = darwin_test::timeOptions();
  ok(Base, Base + 16, Base + 24);
  EXPECT_EQ(bytes(Base, 32), llvm::fromHex(darwin_test::TimeHex));
}

TEST_P(DarwinTimeTest, MissingTimezoneRetainsTimevalButCannotOverrideItsFault) {
  Options->Timezone.reset();
  EXPECT_FALSE(invoke(Base, Base + 16, Base + 24));
  EXPECT_EQ(Result.Diagnostic, diagnostic::TimeZoneMissing);
  EXPECT_EQ(bytes(Base, 16), llvm::fromHex(darwin_test::TimeHex).substr(0, 16));
  EXPECT_EQ(bytes(Base + 16, 16), std::string(16, '\xa5'));
  fill();
  fault(1, Base + 16, Base + 24);
  EXPECT_EQ(bytes(Base, 32), std::string(32, '\xa5'));
}

TEST_P(DarwinTimeTest, LaterFaultsKeepEarlierCopiesAndRetryUsesSameSample) {
  const auto Expected = llvm::fromHex(darwin_test::TimeHex);
  for (auto Bad : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    fill();
    fault(Bad, Base + 16, Base + 24);
    EXPECT_EQ(bytes(Base, 32), std::string(32, '\xa5'));
    fault(Base, Bad, Base + 24);
    EXPECT_EQ(bytes(Base, 16), Expected.substr(0, 16));
    EXPECT_EQ(bytes(Base + 16, 16), std::string(16, '\xa5'));
    fault(Base, Base + 16, Bad);
    EXPECT_EQ(bytes(Base, 24), Expected.substr(0, 24));
    EXPECT_EQ(bytes(Base + 24, 8), std::string(8, '\xa5'));
    ok(Base, Base + 16, Base + 24);
    EXPECT_EQ(bytes(Base, 32), Expected);
  }
}

TEST_P(DarwinTimeTest,
       PartialIndividualCopiesStopWithoutGuessingPrefixEffects) {
  const uint64_t End = Base + Page * 2 - 4;
  const auto Expected = llvm::fromHex(darwin_test::TimeHex);
  for (unsigned Phase = 0; Phase != 3; ++Phase) {
    fill();
    std::array<uint64_t, 3> Addresses{Base, Base + 16, Base + 24};
    Addresses[Phase] = End;
    EXPECT_FALSE(invoke(Addresses[0], Addresses[1], Addresses[2]));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::TimePartialOutput);
    EXPECT_EQ(bytes(End, 4), std::string(4, '\xa5'));
    const size_t Written = Phase == 0 ? 0 : Phase == 1 ? 16 : 24;
    EXPECT_EQ(bytes(Base, Written), Expected.substr(0, Written));
    EXPECT_EQ(bytes(Base + Written, 32 - Written),
              std::string(32 - Written, '\xa5'));
  }
}

TEST_P(DarwinTimeTest, ReadOnlyAndPrivilegedMemoryDoNotBecomeCPUFaults) {
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    fault(Base + Page, 0, 0);
    fault(0, Base + Page, 0);
    fault(0, 0, Base + Page);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
    EXPECT_EQ(bytes(Base + Page, 32), std::string(32, '\xa5'));
  }
}

TEST_P(DarwinTimeTest, AliasesObserveTimevalTimezoneThenAbsoluteWriteOrder) {
  ok(Base, Base, Base);
  EXPECT_EQ(bytes(Base, 16), llvm::fromHex("1032547698badcfef1fb090000000000"));
  EXPECT_EQ(bytes(Base + 16, 8), std::string(8, '\xa5'));
  fill();
  ok(Base, Base + 4, Base + 6);
  EXPECT_EQ(bytes(Base, 16), llvm::fromHex("674523f120fe1032547698badcfe0000"));
}

TEST(DarwinTimeOptions, RejectsInvalidMicroseconds) {
  DarwinTimeOptions Options;
  EXPECT_FALSE(bool(validateTimeOptions(Options)));
  for (auto Invalid : {1000000u, UINT32_MAX}) {
    Options.TimeOfDay = DarwinTimeOfDay{0, Invalid};
    EXPECT_EQ(llvm::toString(validateTimeOptions(Options)),
              diagnostic::TimeMicroseconds);
  }
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinTimeTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
