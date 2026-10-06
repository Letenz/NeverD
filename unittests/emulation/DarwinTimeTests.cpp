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
  std::optional<ServiceResult> mach(ServiceKind Kind, uint64_t Address = 0) {
    auto Out = machTimeService(*Space, Kind, {0, 0, {Address}, std::nullopt},
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

TEST_P(DarwinTimeTest, MachTimebasePreservesExactRatioAndUnalignedCanaries) {
  auto Out = mach(ServiceKind::TimebaseInfo, Base + 1);
  ASSERT_TRUE(Out) << Result.Diagnostic;
  EXPECT_EQ(Out->Value, 0u);
  EXPECT_EQ(bytes(Base + 1, 8), llvm::fromHex(darwin_test::TimebaseHex));
  EXPECT_EQ(bytes(Base, 1), std::string(1, '\xa5'));
  EXPECT_EQ(bytes(Base + 9, 7), std::string(7, '\xa5'));
  Options->Timebase = {250, 6};
  ASSERT_TRUE(mach(ServiceKind::TimebaseInfo, Base));
  EXPECT_EQ(bytes(Base, 8), llvm::fromHex("fa00000006000000"));
  Options->Timebase = {UINT32_MAX, UINT32_MAX};
  ASSERT_TRUE(mach(ServiceKind::TimebaseInfo, Base + 4093));
  EXPECT_EQ(bytes(Base + 4093, 8), std::string(8, '\xff'));
}

TEST_P(DarwinTimeTest, MachTimebaseIgnoresWholeCopyFaultsButNotUnknownInputs) {
  for (auto Bad : {uint64_t(0), uint64_t(1), value::UserLimit, UINT64_MAX}) {
    auto Out = mach(ServiceKind::TimebaseInfo, Bad);
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_EQ(Out->Value, 0u);
    EXPECT_FALSE(Out->Error);
    EXPECT_EQ(bytes(Base, 8), std::string(8, '\xa5'));
  }
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    auto Out = mach(ServiceKind::TimebaseInfo, Base + Page);
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_EQ(Out->Value, 0u);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
    EXPECT_EQ(bytes(Base + Page, 8), std::string(8, '\xa5'));
  }
  Options->Timebase.reset();
  for (auto Address : {Base, uint64_t(0), UINT64_MAX}) {
    EXPECT_FALSE(mach(ServiceKind::TimebaseInfo, Address));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::TimebaseMissing);
  }
  Options.reset();
  EXPECT_FALSE(mach(ServiceKind::TimebaseInfo, Base));
  EXPECT_EQ(Result.Diagnostic, diagnostic::TimebaseMissing);
}

TEST_P(DarwinTimeTest, MachTimebasePartialCopyStopsBeforePublishingAPrefix) {
  const uint64_t End = Base + Page * 2 - 4;
  EXPECT_FALSE(mach(ServiceKind::TimebaseInfo, End));
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(Result.Diagnostic, diagnostic::TimebasePartialOutput);
  EXPECT_EQ(bytes(End, 4), std::string(4, '\xa5'));
}

TEST_P(DarwinTimeTest, MachClocksNeedOnlyTheirOwnObservationAndKeepAllBits) {
  for (auto Kind : {ServiceKind::AbsoluteTime, ServiceKind::ContinuousTime}) {
    Options.emplace();
    auto &Ticks = Kind == ServiceKind::AbsoluteTime
                      ? Options->MachAbsoluteTime
                      : Options->MachContinuousTime;
    for (auto Value : {uint64_t(0), UINT64_MAX}) {
      Ticks = Value;
      auto Out = mach(Kind, UINT64_MAX);
      ASSERT_TRUE(Out) << Result.Diagnostic;
      EXPECT_EQ(Out->Value, Value);
      EXPECT_EQ(bytes(Base, 8), std::string(8, '\xa5'));
    }
    Ticks.reset();
    EXPECT_FALSE(mach(Kind));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Kind == ServiceKind::AbsoluteTime
                                     ? diagnostic::TimeAbsoluteMissing
                                     : diagnostic::TimeContinuousMissing);
  }
}

TEST(DarwinTimeOptions, TimebaseRequiresTwoNonzeroUnsignedFields) {
  DarwinTimeOptions Options;
  for (auto Ratio :
       {DarwinTimebase{0, 1}, DarwinTimebase{1, 0}, DarwinTimebase{0, 0}}) {
    Options.Timebase = Ratio;
    EXPECT_EQ(llvm::toString(validateTimeOptions(Options)),
              diagnostic::TimebaseRatio);
  }
  Options.Timebase = {UINT32_MAX, UINT32_MAX};
  EXPECT_FALSE(bool(validateTimeOptions(Options)));
}

// A transport failure is not a guest EFAULT and cannot be discarded by Mach.
class FailingTimeMemory : public GuestMemory {
public:
  bool FailPreflight;
  unsigned Writes = 0;
  explicit FailingTimeMemory(bool FailPreflight)
      : FailPreflight(FailPreflight) {}
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return failure("unexpected map");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return failure("unexpected protect");
  }
  llvm::Error read(uint64_t, llvm::MutableArrayRef<uint8_t>) override {
    return failure("unexpected read");
  }
  llvm::Error write(uint64_t, llvm::ArrayRef<uint8_t>) override {
    ++Writes;
    return failure("transport write failed");
  }
  llvm::Expected<bool> canAccess(uint64_t, uint64_t, unsigned) const override {
    if (FailPreflight)
      return failure("transport preflight failed");
    return true;
  }
};
TEST(DarwinTimeOptions, MachTimebasePropagatesTransportFailures) {
  for (bool Preflight : {true, false}) {
    FailingTimeMemory Memory(Preflight);
    ProcessResult Result{ProcessProfile::MacOSMachO64,
                         GuestArchitecture::AArch64,
                         ExecutionBackendKind::Unicorn, "failure test"};
    auto Out = machTimeService(Memory, ServiceKind::TimebaseInfo,
                               {0, 0, {0x100000}, std::nullopt},
                               darwin_test::timeOptions(), Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Preflight ? "transport preflight failed"
                        : "transport write failed");
    EXPECT_EQ(Memory.Writes, Preflight ? 0u : 1u);
  }
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinTimeTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
