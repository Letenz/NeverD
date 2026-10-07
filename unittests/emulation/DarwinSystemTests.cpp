//===- DarwinSystemTests.cpp - sysctl widths and ordered copies -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinSystemTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinSystem.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinSystemTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000, Output = Base + 128,
                            Length = Base + 256;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinSystemOptions> Options = darwin_test::systemOptions();
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
  void put(uint64_t Address, llvm::StringRef Bytes) {
    ASSERT_FALSE(
        bool(Space->write(Address, llvm::arrayRefFromStringRef(Bytes))));
  }
  void capacity(uint64_t Value) {
    ASSERT_FALSE(bool(Space->writeInteger(Length, Value, 8)));
  }
  uint64_t length() { return llvm::cantFail(Space->readInteger(Length, 8)); }
  std::string bytes(uint64_t Address, size_t Count) {
    std::vector<uint8_t> Data(Count);
    auto E = Space->read(Address, Data);
    EXPECT_FALSE(bool(E));
    llvm::consumeError(std::move(E));
    return std::string(Data.begin(), Data.end());
  }
  std::optional<ServiceResult> invoke(ServiceKind Kind,
                                      std::array<uint64_t, 6> Arguments) {
    auto Out = systemService(*Space, Page, Kind,
                             {0, 0, Arguments, std::nullopt}, Options, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  std::optional<ServiceResult> named(llvm::StringRef Name,
                                     uint64_t Old = Output,
                                     uint64_t Len = Length, uint64_t New = 0,
                                     uint64_t NewLen = 0) {
    put(Base, Name);
    return invoke(ServiceKind::SysctlByName,
                  {Base, Name.size(), Old, Len, New, NewLen});
  }
  std::optional<ServiceResult> mib(uint32_t Root, uint32_t Leaf,
                                   uint64_t Old = Output) {
    EXPECT_FALSE(bool(Space->writeInteger(Base, Root, 4)));
    EXPECT_FALSE(bool(Space->writeInteger(Base + 4, Leaf, 4)));
    return invoke(ServiceKind::Sysctl, {Base, 2, Old, Length});
  }
  void result(std::optional<ServiceResult> Out, uint64_t Error = 0) {
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_EQ(Out->Error, Error != 0);
    EXPECT_EQ(Out->Value, Error);
  }
};

TEST_P(DarwinSystemTest, NamedAndStableNumericBindingsHaveIndependentBytes) {
  struct Sample {
    const char *Name;
    uint32_t Root, Leaf;
    const char *Hex;
  };
  const Sample Samples[] = {
      {"kern.ostype", 1, 1, "44617277696e00"},
      {"kern.osrelease", 1, 2, "32342e7465737400"},
      {"kern.osrevision", 1, 3, "00000080"},
      {"kern.version", 1, 4, "4e6576657244207669727475616c206b65726e656c00"},
      {"kern.osversion", 1, 65, "56343200"},
      {"hw.machine", 6, 1, "7669727475616c363400"},
      {"hw.model", 6, 2, "5669727475616c4d6f64656c00"},
      {"hw.ncpu", 6, 3, "07000000"},
      {"hw.memsize", 6, 24, "1032547698badcfe"}};
  std::string Combined;
  for (const auto &S : Samples) {
    SCOPED_TRACE(S.Name);
    const auto Expected = llvm::fromHex(S.Hex);
    Combined += Expected;
    for (bool Named : {true, false}) {
      fill();
      capacity(Expected.size());
      result(Named ? named(S.Name, Output + 1)
                   : mib(S.Root, S.Leaf, Output + 1));
      EXPECT_EQ(length(), Expected.size());
      EXPECT_EQ(bytes(Output + 1, Expected.size()), Expected);
      EXPECT_EQ(bytes(Output, 1), std::string(1, '\xa5'));
      EXPECT_EQ(bytes(Output + 1 + Expected.size(), 8), std::string(8, '\xa5'));
    }
  }
  EXPECT_EQ(Combined, llvm::fromHex(darwin_test::SystemHex));
}

TEST_P(DarwinSystemTest, PagePolicyDistinguishesNamedQuadFromLegacyInteger) {
  Options.reset();
  const auto PageHex = Page == 4096 ? "00100000" : "00400000";
  for (unsigned Cap : {0, 3, 4, 5, 7, 8, 9}) {
    for (unsigned Kind : {0, 1, 2}) {
      fill();
      capacity(Cap);
      const unsigned Size = Kind != 0 || Cap == 4 ? 4 : 8;
      result(Kind == 0   ? named("hw.pagesize")
             : Kind == 1 ? named("hw.pagesize_compat")
                         : mib(6, 7),
             Cap < Size ? 12 : 0);
      EXPECT_EQ(length(), Cap < Size ? 0u : Size);
      EXPECT_EQ(bytes(Output, Size),
                Cap < Size ? std::string(Size, '\xa5')
                           : llvm::fromHex(PageHex) + std::string(Size - 4, 0));
      EXPECT_EQ(bytes(Output + Size, 8), std::string(8, '\xa5'));
    }
  }
  capacity(4);
  result(named("hw.pagesize", 0));
  EXPECT_EQ(length(), 8u);
}

TEST_P(DarwinSystemTest,
       QuadNarrowingUsesSignedBitsAndRangeErrorsPreserveLength) {
  for (uint64_t Value :
       {0ULL, 0x7fffffffULL, 0xffffffff80000000ULL, 0xffffffffffffffffULL,
        0x80000000ULL, 0x100000000ULL, 0xffffffff7fffffffULL}) {
    SCOPED_TRACE(Value);
    Options->MemorySize = Value;
    const bool Fits = Value == 0 || Value == INT32_MAX ||
                      Value == 0xffffffff80000000ULL || Value == UINT64_MAX;
    for (bool Named : {true, false}) {
      fill();
      capacity(4);
      result(Named ? named("hw.memsize") : mib(6, 24), Fits ? 0 : 34);
      EXPECT_EQ(length(), 4u);
      EXPECT_EQ(llvm::cantFail(Space->readInteger(Output, 4)),
                Fits ? uint32_t(Value) : 0xa5a5a5a5u);
      EXPECT_EQ(bytes(Output + 4, 8), std::string(8, '\xa5'));
      capacity(7);
      result(named("hw.memsize"), 12);
      EXPECT_EQ(length(), 0u);
      capacity(8);
      result(named("hw.memsize"));
      EXPECT_EQ(llvm::cantFail(Space->readInteger(Output, 8)), Value);
    }
  }
}

TEST_P(DarwinSystemTest, StringCapacitiesSizeOnlyAndNullLengthFollowCopyOrder) {
  for (unsigned Cap = 0; Cap != 10; ++Cap) {
    fill();
    capacity(Cap);
    result(named("kern.ostype"), Cap < 7 ? 12 : 0);
    EXPECT_EQ(length(), Cap < 7 ? 0u : 7u);
    EXPECT_EQ(bytes(Output, 7),
              Cap < 7 ? std::string(7, '\xa5') : std::string("Darwin\0", 7));
    EXPECT_EQ(bytes(Output + 7, 8), std::string(8, '\xa5'));
    capacity(Cap);
    result(named("kern.ostype", 0));
    EXPECT_EQ(length(), 7u);
  }
  fill();
  result(named("kern.ostype", Output, 0), 12);
  result(named("kern.ostype", 0, 0));
  EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
  Options->OSType = "";
  capacity(1);
  result(named("kern.ostype"));
  EXPECT_EQ(length(), 1u);
  EXPECT_EQ(bytes(Output, 2), std::string("\0\xa5", 2));
  Options->OSType = std::string(1023, 'x');
  capacity(1024);
  result(named("kern.ostype", Base + 4093));
  EXPECT_EQ(length(), 1024u);
  EXPECT_EQ(bytes(Base + 4093, 1025),
            std::string(1023, 'x') + std::string("\0\xa5", 2));
}

TEST_P(DarwinSystemTest,
       MissingObservationsAndUnknownKeysNeverAcquireDefaults) {
  Options.reset();
  for (bool Present : {false, true}) {
    if (Present)
      Options.emplace();
    capacity(100);
    EXPECT_FALSE(named("kern.ostype"));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemObservation);
    EXPECT_FALSE(named("kern.ostype", 0, 0));
    EXPECT_FALSE(mib(6, 24));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemObservation);
    EXPECT_FALSE(named("kern.unmodeled"));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
    EXPECT_FALSE(mib(0, 0));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
    EXPECT_EQ(length(), 100u);
    EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
  }
  Options->OSType = "";
  Options->MemorySize = 0;
  Options->OSRevision = 0;
  result(named("kern.ostype"));
  EXPECT_EQ(length(), 1u);
  capacity(8);
  result(mib(6, 24));
  EXPECT_EQ(bytes(Output, 8), std::string(8, 0));
  result(mib(1, 3));
  EXPECT_EQ(length(), 4u);
}

TEST_P(DarwinSystemTest, WritesRequireBothNewArgumentsAndPrecedeValueOrOutput) {
  for (const char *Name :
       {"kern.ostype", "kern.osversion", "hw.memsize", "hw.pagesize"}) {
    capacity(32);
    result(named(Name, Output, Length, UINT64_MAX, 0));
    capacity(32);
    result(named(Name, Output, Length, 0, UINT64_MAX));
    for (bool Missing : {false, true}) {
      if (Missing)
        Options.reset();
      capacity(0);
      const auto Before = bytes(Output, 32);
      result(named(Name, UINT64_MAX, Length, UINT64_MAX, 1), 1);
      EXPECT_EQ(length(), 0u);
      EXPECT_EQ(bytes(Output, 32), Before);
    }
    Options = darwin_test::systemOptions();
  }
}

TEST_P(DarwinSystemTest, LengthLimitsAndHighCarriersPrecedeInputMemory) {
  for (auto Count : {0ULL, 1ULL, 13ULL, 0xffffffffffffffffULL})
    result(invoke(ServiceKind::Sysctl, {UINT64_MAX, Count, 1, 1, 1, 1}), 22);
  for (auto Count : {1024ULL, 0x100000000ULL, 0xffffffffffffffffULL})
    result(invoke(ServiceKind::SysctlByName, {UINT64_MAX, Count, 1, 1, 1, 1}),
           63);
  for (auto Bad : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    result(invoke(ServiceKind::Sysctl, {Bad, 2, 1, 1, 1, 1}), 14);
    result(invoke(ServiceKind::SysctlByName, {Bad, 1, 1, 1, 1, 1}), 14);
    capacity(8);
    result(invoke(ServiceKind::SysctlByName, {Bad, 0, 1, Length, 1, 1}), 2);
    EXPECT_EQ(length(), 8u);
  }
  capacity(4);
  result(mib(6, 3));
  capacity(4);
  result(invoke(ServiceKind::Sysctl,
                {Base, 0xfedcba9800000002ULL, Output, Length}));
  EXPECT_EQ(bytes(Output, 4), llvm::fromHex("07000000"));
  capacity(8);
  EXPECT_FALSE(invoke(ServiceKind::Sysctl, {Base, 12, Output, Length}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
  EXPECT_EQ(length(), 8u);
}

TEST_P(DarwinSystemTest, FirstNULAndOneTrailingDotStillRequireAllInputBytes) {
  for (auto Name :
       {std::string("kern.ostype."), std::string("kern.ostype\0junk", 16),
        std::string("kern.ostype.\0junk", 17)}) {
    capacity(8);
    result(named(Name));
    EXPECT_EQ(bytes(Output, 7), std::string("Darwin\0", 7));
  }
  capacity(8);
  EXPECT_FALSE(named("kern.ostype.."));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemKey);
  std::string Full("kern.ostype\0", 12);
  Full.resize(1023, 'x');
  result(named(Full));
  const auto End = Base + 2 * Page;
  put(End - 12, std::string("kern.ostype\0", 12));
  capacity(8);
  EXPECT_FALSE(
      invoke(ServiceKind::SysctlByName, {End - 12, 13, Output, Length}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPartialInput);
  EXPECT_EQ(length(), 8u);
}

TEST_P(DarwinSystemTest,
       InaccessibleLengthIsExplicitlyOutsideSupportedBoundary) {
  for (auto Bad :
       {uint64_t(1), value::UserLimit, UINT64_MAX, Base + 2 * Page - 4}) {
    EXPECT_FALSE(named("kern.ostype", Output, Bad, 1, 1));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
    EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
  }
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Write | UserAccessible),
        unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    EXPECT_FALSE(named("kern.ostype", Output, Base + Page));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
  }
}

TEST_P(DarwinSystemTest,
       OutputFaultsPreserveLengthAndShortCopyAvoidsOutputFault) {
  Options->MemorySize = 0x80000000;
  capacity(4);
  result(named("hw.memsize", UINT64_MAX), 34);
  EXPECT_EQ(length(), 4u);
  EXPECT_EQ(bytes(Output, 8), std::string(8, '\xa5'));
  for (auto Bad : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    capacity(8);
    result(named("kern.ostype", Bad), 14);
    EXPECT_EQ(length(), 8u);
    capacity(6);
    result(named("kern.ostype", Bad), 12);
    EXPECT_EQ(length(), 0u);
    capacity(8);
    result(named("kern.ostype", Bad, Length, UINT64_MAX, 0), 14);
    EXPECT_EQ(length(), 8u);
    capacity(6);
    result(named("kern.ostype", Bad, Length, UINT64_MAX, 0), 12);
    EXPECT_EQ(length(), 0u);
  }
  const auto End = Base + 2 * Page;
  capacity(8);
  EXPECT_FALSE(named("kern.ostype", End - 4));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPartialOutput);
  EXPECT_EQ(length(), 8u);
  EXPECT_EQ(bytes(End - 4, 4), std::string(4, '\xa5'));
  for (auto Permissions :
       {unsigned(Read | UserAccessible), unsigned(Read | Write)}) {
    ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Permissions)));
    result(named("kern.ostype", Base + Page), 14);
    EXPECT_EQ(length(), 8u);
    ASSERT_FALSE(
        bool(Space->protect(Base + Page, Page, Read | Write | UserAccessible)));
  }
}

TEST_P(DarwinSystemTest, AliasesCaptureInputAndCapacityBeforeDataThenLength) {
  capacity(64);
  result(named("kern.version", Length, Length));
  EXPECT_EQ(length(), 22u);
  EXPECT_EQ(bytes(Length + 8, 14), std::string("irtual kernel\0", 14));
  EXPECT_EQ(bytes(Length + 22, 8), std::string(8, '\xa5'));
  capacity(64);
  result(named("kern.ostype", Base));
  EXPECT_EQ(bytes(Base, 7), std::string("Darwin\0", 7));
  capacity(64);
  result(mib(6, 24, Base));
  EXPECT_EQ(bytes(Base, 8), llvm::fromHex("1032547698badcfe"));
  put(Base, "kern.ostype");
  // Both MIB words also encode a large capacity. The final length overwrites
  // them only after [6,3] has selected the CPU count and copied its value.
  ASSERT_FALSE(bool(Space->writeInteger(Base + 32, 0x300000006ULL, 8)));
  result(invoke(ServiceKind::Sysctl, {Base + 32, 2, Output, Base + 32}));
  EXPECT_EQ(bytes(Output, 4), llvm::fromHex("07000000"));
  EXPECT_EQ(bytes(Base + 32, 8), llvm::fromHex("0400000000000000"));
}

// Failures are transport errors, not invented guest errno. Delegation keeps
// earlier real copies observable when the later length write fails.
class FailingSystemMemory : public GuestMemory {
  GuestMemory &Memory;

public:
  unsigned FailAccess = 0, FailRead = 0, FailWrite = 0;
  mutable unsigned Accesses = 0;
  unsigned Reads = 0, Writes = 0;
  explicit FailingSystemMemory(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return failure("map");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return failure("protect");
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    if (++Accesses == FailAccess)
      return failure("transport access");
    return Memory.canAccess(A, N, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (++Reads == FailRead)
      return failure("transport read");
    return Memory.read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    if (++Writes == FailWrite)
      return failure("transport write");
    return Memory.write(A, B);
  }
};
TEST_P(DarwinSystemTest,
       TransportFailuresKeepCompletedCopiesAndDoNotBecomeErrno) {
  for (unsigned Phase = 0; Phase != 3; ++Phase) {
    for (unsigned At = 1; At <= (Phase == 0 ? 4u : 2u); ++At) {
      fill();
      capacity(8);
      put(Base, "kern.ostype");
      FailingSystemMemory Memory(*Space);
      (Phase == 0   ? Memory.FailAccess
       : Phase == 1 ? Memory.FailRead
                    : Memory.FailWrite) = At;
      auto Out = systemService(
          Memory, Page, ServiceKind::SysctlByName,
          {0, 274, {Base, 11, Output, Length}, std::nullopt}, Options, Result);
      ASSERT_FALSE(bool(Out));
      EXPECT_EQ(llvm::toString(Out.takeError()), Phase == 0 ? "transport access"
                                                 : Phase == 1
                                                     ? "transport read"
                                                     : "transport write");
      const bool Copied = (Phase == 0 && At == 4) || (Phase == 2 && At == 2);
      EXPECT_EQ(bytes(Output, 7),
                Copied ? std::string("Darwin\0", 7) : std::string(7, '\xa5'));
      EXPECT_EQ(length(), 8u);
    }
  }
}

TEST(DarwinSystemOptions, TypedFieldsUseTheSameBoundedContract) {
  auto O = darwin_test::systemOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  for (auto Member :
       {&DarwinSystemOptions::OSType, &DarwinSystemOptions::OSRelease,
        &DarwinSystemOptions::OSVersion, &DarwinSystemOptions::KernelVersion,
        &DarwinSystemOptions::Machine, &DarwinSystemOptions::Model}) {
    for (const auto &Bad : {std::string(1024, 'x'), std::string("x\0y", 3)}) {
      O.*Member = Bad;
      EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
                diagnostic::SystemString);
    }
    O.*Member = std::string(1023, 'x');
    EXPECT_FALSE(bool(validateSystemOptions(O)));
    O.*Member = "";
    EXPECT_FALSE(bool(validateSystemOptions(O)));
  }
  for (auto Bad : {0u, 0x80000000u, UINT32_MAX}) {
    O.CPUCount = Bad;
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::SystemCPUCount);
  }
  O.CPUCount = INT32_MAX;
  O.MemorySize = UINT64_MAX;
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST_P(DarwinSystemTest, ResourceLimitsKeepBothWordsFlagsAndUnalignedGuards) {
  Options = darwin_test::resourceLimitOptions();
  const auto Expected = llvm::fromHex(darwin_test::ResourceLimitsHex);
  ASSERT_EQ(Expected.size(), 144u);
  for (uint32_t Resource = 0; Resource != 9; ++Resource) {
    for (uint32_t Flag : {0u, 0x1000u}) {
      SCOPED_TRACE(Resource | Flag);
      fill();
      result(invoke(ServiceKind::GetRlimit, {Resource | Flag, Output + 1}));
      EXPECT_EQ(bytes(Output + 1, 16), Expected.substr(Resource * 16, 16));
      EXPECT_EQ(bytes(Output, 1), std::string(1, '\xa5'));
      EXPECT_EQ(bytes(Output + 17, 16), std::string(16, '\xa5'));
    }
  }
  EXPECT_EQ(Options->ResourceLimits.size(), 9u);
  EXPECT_EQ(Options->ResourceLimits.at(0).Current, 0u);
  EXPECT_EQ(Options->ResourceLimits.at(0).Maximum, 0u);
  EXPECT_EQ(Options->ResourceLimits.at(1).Current, uint64_t(INT64_MAX));
}

TEST_P(DarwinSystemTest, ResourceSelectorsAndMissingValuesPrecedeAllMemory) {
  for (unsigned Configuration = 0; Configuration != 3; ++Configuration) {
    if (Configuration == 0)
      Options.reset();
    else if (Configuration == 1)
      Options = DarwinSystemOptions{};
    else
      Options = darwin_test::resourceLimitOptions();
    for (uint64_t Selector : {uint64_t(9), uint64_t(0x1009), uint64_t(0x2000),
                              uint64_t(UINT32_MAX)}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                               {0, 194, {Selector, Output}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      result(*Out, value::InvalidArgument);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
    if (Configuration == 2)
      Options->ResourceLimits.erase(8);
    for (uint64_t Selector : {uint64_t(8), uint64_t(0x1008)}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                               {0, 194, {Selector, Output}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      EXPECT_FALSE(*Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceLimitObservation);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, ResourceSelectorUsesLow32BitsAndOnlyThePosixFlag) {
  Options = darwin_test::resourceLimitOptions();
  const uint64_t Selectors[] = {8, 0x1008, 0x100000008ULL,
                                0xffffffff00001008ULL};
  const auto Expected =
      llvm::fromHex(darwin_test::ResourceLimitsHex).substr(128);
  for (auto Selector : Selectors) {
    fill();
    result(invoke(ServiceKind::GetRlimit, {Selector, Output}));
    EXPECT_EQ(bytes(Output, 16), Expected);
    EXPECT_EQ(bytes(Output - 8, 8), std::string(8, '\xa5'));
    EXPECT_EQ(bytes(Output + 16, 8), std::string(8, '\xa5'));
  }
  fill();
  result(invoke(ServiceKind::GetRlimit, {0x2008, Output}),
         value::InvalidArgument);
  EXPECT_EQ(bytes(Output, 16), std::string(16, '\xa5'));
}

TEST_P(DarwinSystemTest,
       ResourcePairsCrossMappedPagesAndRejectUnwritableOutput) {
  Options = darwin_test::resourceLimitOptions();
  const auto Expected =
      llvm::fromHex(darwin_test::ResourceLimitsHex).substr(128);
  result(invoke(ServiceKind::GetRlimit, {8, Base + Page - 7}));
  EXPECT_EQ(bytes(Base + Page - 7, 16), Expected);
  EXPECT_EQ(bytes(Base + Page - 15, 8), std::string(8, '\xa5'));
  EXPECT_EQ(bytes(Base + Page + 9, 8), std::string(8, '\xa5'));
  fill();
  const uint64_t Faults[] = {0, 1, value::UserLimit, UINT64_MAX,
                             Base + Page * 2};
  for (auto Fault : Faults) {
    result(invoke(ServiceKind::GetRlimit, {8, Fault}), value::BadAddress);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  result(invoke(ServiceKind::GetRlimit, {8, Base + Page}), value::BadAddress);
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, PartialResourcePairNeverPublishesEitherWord) {
  Options = darwin_test::resourceLimitOptions();
  for (bool ReadOnly : {false, true}) {
    fill();
    if (ReadOnly)
      ASSERT_FALSE(
          bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
    const auto Output = ReadOnly ? Base + Page - 8 : Base + Page * 2 - 8;
    auto Out = invoke(ServiceKind::GetRlimit, {8, Output});
    EXPECT_FALSE(Out);
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceLimitPartialOutput);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
}

TEST_P(DarwinSystemTest, ResourcePairTransportFailureDoesNotInventGuestErrno) {
  Options = darwin_test::resourceLimitOptions();
  for (unsigned Failure = 0; Failure != 3; ++Failure) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Failure < 2)
      Memory.FailAccess = Failure + 1;
    else
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                             {0, 194, {8, Base + Page - 8}, std::nullopt},
                             Options, Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Failure < 2 ? "transport access" : "transport write");
    EXPECT_EQ(Memory.Accesses, Failure < 2 ? Failure + 1 : 2u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Failure < 2 ? 0u : 1u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetRlimit,
                           {0, 194, {8, Base + Page - 8}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  result(*Out);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 1u);
  std::string Expected(Page * 2, '\xa5');
  Expected.replace(Page - 8, 16,
                   llvm::fromHex(darwin_test::ResourceLimitsHex).substr(128));
  EXPECT_EQ(bytes(Base, Page * 2), Expected);
}

TEST(DarwinSystemOptions, ResourcePairsRequireCanonicalKeysAndBoundedValues) {
  for (auto Resource : {9u, 4096u, UINT32_MAX}) {
    auto O = darwin_test::resourceLimitOptions();
    O.ResourceLimits[Resource] = {0, 0};
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::ResourceLimitOption);
  }
  for (const DarwinResourceLimit Bad :
       {DarwinResourceLimit{1, 0},
        DarwinResourceLimit{0, uint64_t(INT64_MAX) + 1},
        DarwinResourceLimit{UINT64_MAX, UINT64_MAX}}) {
    auto O = darwin_test::resourceLimitOptions();
    O.ResourceLimits[8] = Bad;
    EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
              diagnostic::ResourceLimitOption);
  }
  auto O = darwin_test::resourceLimitOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.ResourceLimits.clear();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST_P(DarwinSystemTest, UsagePreservesSignedWordsPaddingAndUnalignedGuards) {
  Options = darwin_test::resourceUsageOptions();
  const auto Encoded = llvm::fromHex(darwin_test::ResourceUsageHex);
  ASSERT_EQ(Encoded.size(), 288u);
  const uint64_t Selectors[] = {0, UINT32_MAX, 0x100000000ULL,
                                0x12345678ffffffffULL};
  for (auto Selector : Selectors) {
    SCOPED_TRACE(Selector);
    fill();
    result(invoke(ServiceKind::GetRusage, {Selector, Output + 1}));
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Output + 1 - Base, 144,
                     Encoded.substr(uint32_t(Selector) == 0 ? 0 : 144, 144));
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
    EXPECT_EQ(bytes(Output + 13, 4), std::string(4, '\0'));
    EXPECT_EQ(bytes(Output + 29, 4), std::string(4, '\0'));
  }
}

TEST_P(DarwinSystemTest, UsageSnapshotsAreIndependentAndNeverDefaultMissing) {
  for (bool AbsentSystem : {true, false}) {
    if (AbsentSystem)
      Options.reset();
    else
      Options = DarwinSystemOptions{};
    for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, ServiceKind::GetRusage,
                               {0, 117, {Selector, Output}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      EXPECT_FALSE(*Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceUsageObservation);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  for (bool Self : {true, false}) {
    Options = darwin_test::resourceUsageOptions();
    if (Self)
      Options->ResourceUsageChildren.reset();
    else
      Options->ResourceUsageSelf.reset();
    fill();
    result(invoke(ServiceKind::GetRusage,
                  {Self ? 0u : uint64_t(UINT32_MAX), Output}));
    EXPECT_EQ(bytes(Output, 144), llvm::fromHex(darwin_test::ResourceUsageHex)
                                      .substr(Self ? 0 : 144, 144));
    fill();
    FailingSystemMemory Memory(*Space);
    Memory.FailAccess = Memory.FailWrite = 1;
    auto Out = systemService(
        Memory, Page, ServiceKind::GetRusage,
        {0, 117, {Self ? uint64_t(UINT32_MAX) : 0u, Output}, std::nullopt},
        Options, Result);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    EXPECT_FALSE(*Out);
    EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceUsageObservation);
    EXPECT_EQ(Memory.Accesses, 0u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, 0u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    Options = DarwinSystemOptions{};
    (Self ? Options->ResourceUsageSelf : Options->ResourceUsageChildren) =
        DarwinResourceUsage{};
    result(invoke(ServiceKind::GetRusage,
                  {Self ? 0u : uint64_t(UINT32_MAX), Output}));
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Output - Base, 144, std::string(144, '\0'));
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
  }
}

TEST_P(DarwinSystemTest, InvalidUsageSelectorsPrecedeEveryMemoryAccess) {
  const uint64_t Selectors[] = {1, 0x1000, uint64_t(UINT32_MAX) - 1,
                                0xffffffff00000001ULL};
  for (unsigned Configuration = 0; Configuration != 3; ++Configuration) {
    if (Configuration == 0)
      Options.reset();
    else if (Configuration == 1)
      Options = DarwinSystemOptions{};
    else
      Options = darwin_test::resourceUsageOptions();
    for (auto Selector : Selectors) {
      for (auto OutputAddress : {uint64_t(0), Output}) {
        FailingSystemMemory Memory(*Space);
        Memory.FailAccess = Memory.FailWrite = 1;
        auto Out = systemService(
            Memory, Page, ServiceKind::GetRusage,
            {0, 117, {Selector, OutputAddress}, std::nullopt}, Options, Result);
        ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
        result(*Out, value::InvalidArgument);
        EXPECT_EQ(Memory.Accesses, 0u);
        EXPECT_EQ(Memory.Reads, 0u);
        EXPECT_EQ(Memory.Writes, 0u);
      }
    }
  }
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
}

TEST_P(DarwinSystemTest, UsageCrossesPagesAndRejectsWhollyUnwritableOutput) {
  Options = darwin_test::resourceUsageOptions();
  for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
    fill();
    result(invoke(ServiceKind::GetRusage, {Selector, Base + Page - 71}));
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Page - 71, 144,
                     llvm::fromHex(darwin_test::ResourceUsageHex)
                         .substr(Selector == 0 ? 0 : 144, 144));
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
    fill();
    const uint64_t Faults[] = {0, 1, value::UserLimit, UINT64_MAX,
                               Base + Page * 2};
    for (auto Fault : Faults) {
      result(invoke(ServiceKind::GetRusage, {Selector, Fault}),
             value::BadAddress);
      EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    }
  }
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
    result(invoke(ServiceKind::GetRusage, {Selector, Base + Page}),
           value::BadAddress);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
}

TEST_P(DarwinSystemTest, PartialUsageNeverPublishesEvenANativeWritablePrefix) {
  Options = darwin_test::resourceUsageOptions();
  for (bool ReadOnly : {false, true}) {
    fill();
    if (ReadOnly)
      ASSERT_FALSE(
          bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
    const auto Address = ReadOnly ? Base + Page - 72 : Base + Page * 2 - 72;
    for (auto Selector : {uint64_t(0), uint64_t(UINT32_MAX)}) {
      auto Out = invoke(ServiceKind::GetRusage, {Selector, Address});
      EXPECT_FALSE(Out);
      EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(Result.Diagnostic, diagnostic::ResourceUsagePartialOutput);
      EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
    }
  }
}

TEST_P(DarwinSystemTest, UsageTransportErrorsRemainErrorsAndSuccessCopiesOnce) {
  Options = darwin_test::resourceUsageOptions();
  for (unsigned Failure = 0; Failure != 3; ++Failure) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Failure < 2)
      Memory.FailAccess = Failure + 1;
    else
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetRusage,
                             {0, 117, {0, Base + Page - 71}, std::nullopt},
                             Options, Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Failure < 2 ? "transport access" : "transport write");
    EXPECT_EQ(Memory.Accesses, Failure < 2 ? Failure + 1 : 2u);
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Failure < 2 ? 0u : 1u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetRusage,
                           {0, 117, {0, Base + Page - 71}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  result(*Out);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 1u);
  std::string Expected(Page * 2, '\xa5');
  Expected.replace(Page - 71, 144,
                   llvm::fromHex(darwin_test::ResourceUsageHex).substr(0, 144));
  EXPECT_EQ(bytes(Base, Page * 2), Expected);
}

TEST(DarwinSystemOptions,
     UsageMicrosecondsAreBoundedAndSignedWordsStayLossless) {
  auto O = darwin_test::resourceUsageOptions();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  EXPECT_EQ(O.ResourceUsageSelf->UserSeconds, INT64_MIN);
  EXPECT_EQ(O.ResourceUsageSelf->Counters[0], INT64_MAX);
  EXPECT_EQ(O.ResourceUsageSelf->Counters[1], INT64_MIN);
  for (bool Self : {true, false}) {
    for (bool User : {true, false}) {
      for (auto Bad : {1000000u, UINT32_MAX}) {
        O = darwin_test::resourceUsageOptions();
        auto &Usage = Self ? *O.ResourceUsageSelf : *O.ResourceUsageChildren;
        (User ? Usage.UserMicroseconds : Usage.SystemMicroseconds) = Bad;
        EXPECT_EQ(llvm::toString(validateSystemOptions(O)),
                  diagnostic::ResourceUsageOption);
      }
    }
  }
  O = DarwinSystemOptions{};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.ResourceUsageSelf = DarwinResourceUsage{};
  O.ResourceUsageChildren = DarwinResourceUsage{};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

TEST_P(DarwinSystemTest, CredentialScalarsShareSelectionWithoutMemoryAccess) {
  const ServiceKind Kinds[] = {ServiceKind::GetUID, ServiceKind::GetEUID,
                               ServiceKind::GetGID, ServiceKind::GetEGID};
  for (unsigned Configuration = 0; Configuration != 4; ++Configuration) {
    if (!Configuration)
      Options.reset();
    else
      Options = DarwinSystemOptions{};
    if (Configuration == 2)
      Options->Credentials = DarwinCredentials{0, 7, 9, 11, {}};
    if (Configuration == 3)
      Options->Credentials = DarwinCredentials{};
    const uint32_t Expected[] = {0, 7, 9, 11};
    for (unsigned I = 0; I != 4; ++I) {
      FailingSystemMemory Memory(*Space);
      Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
      auto Out = systemService(Memory, Page, Kinds[I],
                               {0, 0, {UINT64_MAX, UINT64_MAX}, std::nullopt},
                               Options, Result);
      ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
      ASSERT_TRUE(*Out);
      EXPECT_FALSE((**Out).Error);
      EXPECT_EQ((**Out).Value, Configuration < 2    ? 1000u
                               : Configuration == 2 ? Expected[I]
                                                    : 0u);
      EXPECT_EQ(Memory.Accesses, 0u);
      EXPECT_EQ(Memory.Reads, 0u);
      EXPECT_EQ(Memory.Writes, 0u);
    }
  }
}
TEST_P(DarwinSystemTest, GroupsPreserveDuplicatesAndOnlyCopyActualCount) {
  Options = darwin_test::credentialOptions();
  const auto Data = llvm::fromHex(darwin_test::GroupsHex);
  ASSERT_EQ(Data.size(), 20u);
  for (auto Capacity : {5ULL, 16ULL, 0x1000ULL, 0x7fffffffULL,
                        0x1234567800000005ULL, 0x1234567800001000ULL})
    for (auto OutAddress : {Output + 1, Base + Page - 9}) {
      fill();
      auto Out = invoke(ServiceKind::GetGroups, {Capacity, OutAddress});
      ASSERT_TRUE(Out);
      EXPECT_FALSE(Out->Error);
      EXPECT_EQ(Out->Value, 5u);
      std::string Expected(Page * 2, '\xa5');
      Expected.replace(OutAddress - Base, Data.size(), Data);
      EXPECT_EQ(bytes(Base, Page * 2), Expected);
    }
  EXPECT_EQ(Options->Credentials->EffectiveGID, 404u);
  EXPECT_EQ(*Options->Credentials->GroupAccessList,
            (std::vector<uint32_t>{404, 0, INT32_MAX, 7, 7}));
}
TEST_P(DarwinSystemTest, GroupsMaximumAndExplicitRootAreNotDefaulted) {
  for (bool Maximum : {false, true}) {
    Options = DarwinSystemOptions{};
    Options->Credentials = DarwinCredentials{};
    Options->Credentials->EffectiveGID = Maximum ? INT32_MAX : 0;
    Options->Credentials->GroupAccessList =
        Maximum ? std::vector<uint32_t>{INT32_MAX, 0, 1, 2,  3,  4,  5,  6,
                                        7,         8, 9, 10, 11, 12, 13, 14}
                : std::vector<uint32_t>{0};
    ASSERT_FALSE(bool(validateSystemOptions(*Options)));
    const auto Data = llvm::fromHex(
        Maximum
            ? "ffffff7f00000000010000000200000003000000040000000500000006000000"
              "0700000008000000090000000a0000000b0000000c0000000d0000000e000000"
            : "00000000");
    fill();
    auto Out = invoke(ServiceKind::GetGroups, {0x1000, Output + 1});
    ASSERT_TRUE(Out);
    EXPECT_FALSE(Out->Error);
    EXPECT_EQ(Out->Value, Maximum ? 16u : 1u);
    std::string Expected(Page * 2, '\xa5');
    Expected.replace(Output + 1 - Base, Data.size(), Data);
    EXPECT_EQ(bytes(Base, Page * 2), Expected);
  }
}
TEST_P(DarwinSystemTest, GroupEarlyDecisionsNeverAccessOutput) {
  for (unsigned Configuration = 0; Configuration != 4; ++Configuration) {
    if (!Configuration)
      Options.reset();
    else
      Options = DarwinSystemOptions{};
    if (Configuration == 2)
      Options->Credentials = DarwinCredentials{};
    if (Configuration == 3)
      Options = darwin_test::credentialOptions();
    for (auto Capacity :
         {0ULL, 0xffffffff00000000ULL, 1ULL, 4ULL, 0x1234567800000004ULL,
          0xffffffffULL, 0x80000000ULL, 0xffffffff80000000ULL})
      for (auto Address : {uint64_t(0), Output, UINT64_MAX}) {
        FailingSystemMemory Memory(*Space);
        Memory.FailAccess = Memory.FailRead = Memory.FailWrite = 1;
        auto Out = systemService(Memory, Page, ServiceKind::GetGroups,
                                 {0, 79, {Capacity, Address}, std::nullopt},
                                 Options, Result);
        ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
        bool Invalid = uint32_t(Capacity) & 0x80000000u;
        bool Short = Configuration == 3 && uint32_t(Capacity) != 0 &&
                     uint32_t(Capacity) < 5;
        if (Invalid || Short) {
          ASSERT_TRUE(*Out);
          EXPECT_TRUE((**Out).Error);
          EXPECT_EQ((**Out).Value, 22u);
        } else if (Configuration != 3) {
          EXPECT_FALSE(*Out);
          EXPECT_EQ(Result.Diagnostic, diagnostic::GroupObservation);
        } else {
          ASSERT_TRUE(*Out);
          EXPECT_FALSE((**Out).Error);
          EXPECT_EQ((**Out).Value, 5u);
        }
        EXPECT_EQ(Memory.Accesses, 0u);
        EXPECT_EQ(Memory.Reads, 0u);
        EXPECT_EQ(Memory.Writes, 0u);
      }
  }
}
TEST_P(DarwinSystemTest, GroupFaultAndPartialCopiesNeverPublishBytes) {
  Options = darwin_test::credentialOptions();
  for (auto Address : {uint64_t(0), uint64_t(1), value::UserLimit, UINT64_MAX,
                       Base + Page * 2}) {
    fill();
    result(invoke(ServiceKind::GetGroups, {0x1000, Address}), 14);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  fill();
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  result(invoke(ServiceKind::GetGroups, {0x1000, Base + Page}), 14);
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  EXPECT_FALSE(invoke(ServiceKind::GetGroups, {0x1000, Base + Page - 10}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::GroupPartialOutput);
  EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  ASSERT_FALSE(bool(Space->unmap(Base + Page, Page)));
  EXPECT_FALSE(invoke(ServiceKind::GetGroups, {16, Base + Page - 10}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::GroupPartialOutput);
  EXPECT_EQ(bytes(Base, Page), std::string(Page, '\xa5'));
}
TEST_P(DarwinSystemTest, GroupTransportErrorsAndSingleSuccessfulCopy) {
  Options = darwin_test::credentialOptions();
  for (unsigned Failure = 0; Failure != 3; ++Failure) {
    fill();
    FailingSystemMemory Memory(*Space);
    if (Failure < 2)
      Memory.FailAccess = Failure + 1;
    else
      Memory.FailWrite = 1;
    auto Out = systemService(Memory, Page, ServiceKind::GetGroups,
                             {0, 79, {5, Base + Page - 9}, std::nullopt},
                             Options, Result);
    ASSERT_FALSE(bool(Out));
    EXPECT_EQ(llvm::toString(Out.takeError()),
              Failure < 2 ? "transport access" : "transport write");
    EXPECT_EQ(Memory.Reads, 0u);
    EXPECT_EQ(Memory.Writes, Failure < 2 ? 0u : 1u);
    EXPECT_EQ(bytes(Base, Page * 2), std::string(Page * 2, '\xa5'));
  }
  fill();
  FailingSystemMemory Memory(*Space);
  auto Out = systemService(Memory, Page, ServiceKind::GetGroups,
                           {0, 79, {0x1000, Base + Page - 9}, std::nullopt},
                           Options, Result);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  ASSERT_TRUE(*Out);
  EXPECT_FALSE((**Out).Error);
  EXPECT_EQ((**Out).Value, 5u);
  EXPECT_EQ(Memory.Accesses, 2u);
  EXPECT_EQ(Memory.Reads, 0u);
  EXPECT_EQ(Memory.Writes, 1u);
  std::string Expected(Page * 2, '\xa5');
  Expected.replace(Base + Page - 9 - Base, 20,
                   llvm::fromHex(darwin_test::GroupsHex));
  EXPECT_EQ(bytes(Base, Page * 2), Expected);
}
TEST_P(DarwinSystemTest,
       SysctlWriteDecisionUsesEffectiveIdentityAfterPreflight) {
  for (unsigned Identity = 0; Identity != 3; ++Identity)
    for (bool Missing : {false, true})
      for (bool Named : {false, true}) {
        Options = darwin_test::systemOptions();
        if (Identity)
          Options->Credentials = DarwinCredentials{
              Identity == 1 ? 0u : 7u, Identity == 1 ? 7u : 0u, 0, 0, {}};
        if (Missing)
          Options->OSVersion.reset();
        fill();
        capacity(0);
        std::optional<ServiceResult> Out;
        if (Named)
          Out = named("kern.osversion", UINT64_MAX, Length, UINT64_MAX, 1);
        else {
          ASSERT_FALSE(bool(Space->writeInteger(Base, 1, 4)));
          ASSERT_FALSE(bool(Space->writeInteger(Base + 4, 65, 4)));
          Out = invoke(ServiceKind::Sysctl,
                       {Base, 2, UINT64_MAX, Length, UINT64_MAX, 1});
        }
        if (Identity == 2) {
          EXPECT_FALSE(Out);
          EXPECT_EQ(Result.Diagnostic, diagnostic::SystemPrivilegedWrite);
        } else
          result(Out, 1);
        EXPECT_EQ(length(), 0u);
        EXPECT_EQ(bytes(Output, 32), std::string(32, '\xa5'));
        capacity(32);
        result(named("kern.ostype", UINT64_MAX, Length, UINT64_MAX, 1), 1);
      }
  Options = darwin_test::systemOptions();
  Options->Credentials = DarwinCredentials{};
  capacity(8);
  result(named("kern.osversion", Output, Length, UINT64_MAX, 0));
  EXPECT_EQ(bytes(Output, 4), std::string("V42\0", 4));
}
TEST_P(DarwinSystemTest, RootSysctlCannotSkipNameOrLengthPreflight) {
  Options = darwin_test::systemOptions();
  Options->Credentials = DarwinCredentials{};
  result(invoke(ServiceKind::SysctlByName,
                {UINT64_MAX, 14, UINT64_MAX, Length, UINT64_MAX, 1}),
         14);
  result(invoke(ServiceKind::Sysctl,
                {UINT64_MAX, 2, UINT64_MAX, Length, UINT64_MAX, 1}),
         14);
  EXPECT_FALSE(named("kern.osversion", UINT64_MAX, 1, UINT64_MAX, 1));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
  ASSERT_FALSE(bool(Space->writeInteger(Base, 1, 4)));
  ASSERT_FALSE(bool(Space->writeInteger(Base + 4, 65, 4)));
  EXPECT_FALSE(
      invoke(ServiceKind::Sysctl, {Base, 2, UINT64_MAX, 1, UINT64_MAX, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SystemLengthMemory);
  EXPECT_EQ(bytes(Output, 32), std::string(32, '\xa5'));
}
TEST(DarwinSystemOptions, CredentialsRequireBoundedCoherentGroups) {
  auto O = darwin_test::credentialOptions();
  ASSERT_FALSE(bool(validateSystemOptions(O)));
  for (auto Member :
       {&DarwinCredentials::RealUID, &DarwinCredentials::EffectiveUID,
        &DarwinCredentials::RealGID, &DarwinCredentials::EffectiveGID})
    for (auto ID : {0x80000000u, UINT32_MAX}) {
      auto Bad = O;
      (*Bad.Credentials).*Member = ID;
      EXPECT_EQ(llvm::toString(validateSystemOptions(Bad)),
                diagnostic::CredentialOption);
    }
  for (auto Groups : {std::vector<uint32_t>{}, std::vector<uint32_t>(17, 404),
                      std::vector<uint32_t>{0, 404},
                      std::vector<uint32_t>{404, UINT32_MAX}}) {
    auto Bad = O;
    Bad.Credentials->GroupAccessList = Groups;
    EXPECT_EQ(llvm::toString(validateSystemOptions(Bad)),
              diagnostic::CredentialOption);
  }
  O.Credentials =
      DarwinCredentials{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX,
                        std::vector<uint32_t>{INT32_MAX, 0, INT32_MAX}};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.Credentials->GroupAccessList.reset();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.Credentials = DarwinCredentials{0, 0, 0, 0, std::vector<uint32_t>{0}};
  EXPECT_FALSE(bool(validateSystemOptions(O)));
  O.Credentials.reset();
  EXPECT_FALSE(bool(validateSystemOptions(O)));
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinSystemTest,
                         testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
