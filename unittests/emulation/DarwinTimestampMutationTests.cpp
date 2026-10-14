//===- DarwinTimestampMutationTests.cpp - Declared timestamps ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinDirectoryTimestampTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinTimestampMutationTest
    : public testing::TestWithParam<std::tuple<uint64_t, bool>> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  bool FDRoute;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::optional<DarwinTimeOptions> Time;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "timestamp mutation"};
  void SetUp() override {
    std::tie(Page, FDRoute) = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::timestampOptions();
    Credentials = darwin_test::modeCredentials();
    reset();
  }
  void reset() {
    if (Options)
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(
        *Space, Options, process_defaults::Output, 501, Credentials);
  }
  uint64_t path(llvm::StringRef Text, uint64_t Address = Base) {
    const auto Bytes = Text.str() + '\0';
    llvm::cantFail(Space->write(
        Address,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.data()),
                                Bytes.size())));
    return Address;
  }
  uint64_t input(int64_t AS = 11, int32_t AU = 12, int64_t MS = 13,
                 int32_t MU = 14, uint64_t Address = Base + 512) {
    std::array<uint8_t, 32> B;
    B.fill(0xa5);
    llvm::support::endian::write64le(B.data(), uint64_t(AS));
    llvm::support::endian::write32le(B.data() + 8, uint32_t(AU));
    llvm::support::endian::write32le(B.data() + 12, UINT32_MAX);
    llvm::support::endian::write64le(B.data() + 16, uint64_t(MS));
    llvm::support::endian::write32le(B.data() + 24, uint32_t(MU));
    llvm::support::endian::write32le(B.data() + 28, 0x13579bdf);
    llvm::cantFail(Space->write(Address, B));
    return Address;
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result =
        ProcessResult{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                      ExecutionBackendKind::Unicorn, "timestamp mutation"};
    auto Out = Files->handle(K, {0, 0, A, {}}, Result, std::nullopt, Time);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  uint64_t ok(ServiceKind K, std::array<uint64_t, 6> A) {
    auto O = invoke(K, A);
    EXPECT_TRUE(O) << Result.Diagnostic;
    EXPECT_FALSE(O && O->Error);
    return O ? O->Value : UINT64_MAX;
  }
  void error(ServiceKind K, std::array<uint64_t, 6> A, uint32_t Errno) {
    auto O = invoke(K, A);
    ASSERT_TRUE(O) << Result.Diagnostic;
    EXPECT_TRUE(O->Error);
    EXPECT_EQ(O->Value, Errno);
  }
  void refused(ServiceKind K, std::array<uint64_t, 6> A, const char *Reason) {
    EXPECT_FALSE(invoke(K, A));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Reason);
  }
  ServiceKind route() const {
    return FDRoute ? ServiceKind::Futimes : ServiceKind::Utimes;
  }
  std::array<uint64_t, 6> args(uint64_t FD, uint64_t Pointer) {
    return {FDRoute ? FD : path("/data"),
            Pointer,
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX};
  }
  uint64_t open(llvm::StringRef Name = "/data", uint32_t Flags = 0) {
    return ok(ServiceKind::Open, {path(Name), Flags, 0600});
  }
  using Record = std::array<uint8_t, 160>;
  Record status(uint64_t FD) {
    Record B;
    B.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, B));
    EXPECT_EQ(ok(ServiceKind::Fstat64, {FD, Base + Page + 8}), 0u);
    llvm::cantFail(Space->read(Base + Page, B));
    for (unsigned I = 0; I != 8; ++I) {
      EXPECT_EQ(B[I], 0xa5);
      EXPECT_EQ(B[152 + I], 0xa5);
    }
    return B;
  }
  static void time(Record &R, unsigned Offset, int64_t S, int64_t N) {
    llvm::support::endian::write64le(R.data() + 8 + Offset, uint64_t(S));
    llvm::support::endian::write64le(R.data() + 16 + Offset, uint64_t(N));
  }
  Record changed(Record R, int64_t AS, int64_t AN, int64_t MS, int64_t MN,
                 int64_t BS = 7, int64_t BN = 8, int64_t CS = 111,
                 int64_t CN = 112) {
    time(R, 32, AS, AN);
    time(R, 48, MS, MN);
    time(R, 64, CS, CN);
    time(R, 80, BS, BN);
    return R;
  }
  void policies(bool Created = false) {
    Options->WritableFiles.insert("/data");
    Options->MutationPolicies["/data"] = {512, {101, 102}};
    Options->MutableDirectories.insert("/");
    Options->DirectoryMutationPolicies["/"] = {16, {91, 92}};
    Options->InitialUmask = 022;
    Options->CreationPolicy =
        DarwinFileCreationPolicy{100,
                                 512,
                                 9,
                                 {41, 42},
                                 {512, {101, 102}},
                                 DarwinNamespaceCreationPolicy{512, 32, 7}};
    Options->TimestampMutations->CreatedRegularObjects = Created;
    reset();
  }
};
TEST(DarwinTimestampMutationOptions, AdmissionDefaultsAndBothClosedDomains) {
  const auto Initial = darwin_test::timestampOptions();
  EXPECT_FALSE(Initial.TimestampMutations->CreatedRegularObjects);
  EXPECT_FALSE(bool(validateFileOptions(Initial)));
  for (unsigned I = 0; I != 10; ++I) {
    auto O = Initial;
    switch (I) {
    case 0:
      O.TimestampMutations->Objects["/missing"] = {1, 2};
      break;
    case 1:
      O.TimestampMutations->Objects["/missing-directory"] = {1, 2};
      break;
    case 2:
      O.Metadata.erase("/data");
      break;
    case 3:
      O.Metadata["/data"].Flags = 1;
      break;
    case 4:
      O.Metadata["/data"].Mode |= 04000;
      break;
    case 5:
      O.Metadata["/data"].LinkCount = 2;
      break;
    case 6:
      O.TimestampMutations->Objects["/data"].Nanoseconds = -1;
      break;
    case 7:
      O.TimestampMutations->Objects["/data"].Nanoseconds = 1000000000;
      break;
    case 8:
      O.Files["/alias"] = O.Files["/data"];
      O.Metadata["/alias"] = O.Metadata["/data"];
      break;
    case 9:
      O.TimestampMutations->CreatedRegularObjects = true;
      break;
    }
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << I;
    llvm::consumeError(std::move(E));
  }
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries}) {
    auto O = Initial;
    O.Authorization = A;
    O.TimestampMutations->Objects.clear();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST_P(DarwinTimestampMutationTest,
       LiteralRecordReadOnlyDupOffsetsFlagsAndInput) {
  const auto A = open(), B = open(), D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(status(A), darwin_test::timestampRecord());
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 3, 0}), 3u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 2, 1}), 0u);
  const auto P = input();
  std::array<uint8_t, 32> Before, After;
  llvm::cantFail(Space->read(P, Before));
  EXPECT_EQ(ok(route(), args(FDRoute ? (0xffffffff00000000ULL | D) : D, P)),
            0u);
  const auto Wanted =
      changed(darwin_test::timestampRecord(), 11, 12000, 13, 14000);
  EXPECT_EQ(status(A), Wanted);
  EXPECT_EQ(status(B), Wanted);
  EXPECT_EQ(status(D), Wanted);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 0u);
  llvm::cantFail(Space->read(P, After));
  EXPECT_EQ(Before, After);
  EXPECT_EQ(ok(ServiceKind::Pread, {A, Base + 1024, 6, 0}), 6u);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Base + 1024, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinTimestampMutationTest, SignedMicrosecondsAndSaturatedSeconds) {
  struct Case {
    int64_t S;
    int32_t U;
    int64_t Seconds, Nanos;
  };
  for (const auto &V :
       std::array<Case, 8>{{{0, -1, -1, 999999000},
                            {1, 1000000, 2, 0},
                            {0, INT32_MIN, -2148, 516352000},
                            {0, INT32_MAX, 2147, 483647000},
                            {INT64_MAX, INT32_MAX, 9223372036, 854775807},
                            {INT64_MIN, INT32_MIN, -9223372037, 145224192},
                            {-1, 0, -1, 0},
                            {-2, 2000000, 0, 0}}}) {
    reset();
    const auto FD = open();
    EXPECT_EQ(ok(route(), args(FD, input(V.S, V.U, V.S, V.U))), 0u);
    const bool Earlier = V.Seconds >= 0 && V.Seconds < 7;
    EXPECT_EQ(status(FD),
              changed(darwin_test::timestampRecord(), V.Seconds, V.Nanos,
                      V.Seconds, V.Nanos, Earlier ? V.Seconds : 7,
                      Earlier ? V.Nanos : 8));
  }
}
TEST_P(DarwinTimestampMutationTest,
       BirthBoundaryIncludesZeroAndExcludesNegative) {
  struct Case {
    int64_t S;
    int32_t U;
    int64_t BS, BN;
  };
  for (const auto &V : std::array<Case, 6>{{{0, 0, 0, 0},
                                            {0, 1, 0, 1000},
                                            {0, -1, 7, 8},
                                            {-1, 0, 7, 8},
                                            {7, 0, 7, 0},
                                            {7, 1, 7, 8}}}) {
    reset();
    const auto FD = open();
    EXPECT_EQ(ok(route(), args(FD, input(11, 12, V.S, V.U))), 0u);
    const auto MS = V.U < 0 ? -1 : V.S;
    const auto MN = V.U < 0 ? 999999000 : int64_t(V.U) * 1000;
    EXPECT_EQ(status(FD), changed(darwin_test::timestampRecord(), 11, 12000, MS,
                                  MN, V.BS, V.BN));
  }
}
TEST_P(DarwinTimestampMutationTest,
       CurrentTimeUsesOnlyDeclaredSampleIncludingZero) {
  const auto FD = open();
  const auto Before = status(FD);
  refused(route(), args(FD, 0), diagnostic::TimeOfDayMissing);
  EXPECT_EQ(status(FD), Before);
  Time.emplace();
  Time->MachAbsoluteTime = 1;
  refused(route(), args(FD, 0), diagnostic::TimeOfDayMissing);
  Time->TimeOfDay = DarwinTimeOfDay{0, 0};
  EXPECT_EQ(ok(route(), args(FD, 0)), 0u);
  EXPECT_EQ(status(FD), changed(Before, 0, 0, 0, 0, 0, 0));
  Time->TimeOfDay = DarwinTimeOfDay{19, 20};
  EXPECT_EQ(ok(route(), args(FD, 0)), 0u);
  EXPECT_EQ(status(FD), changed(Before, 19, 20000, 19, 20000, 0, 0));
}
TEST_P(DarwinTimestampMutationTest, CopyAndFDAndPathPrecedenceBeforeEffects) {
  const auto FD = open();
  const auto Before = status(FD);
  error(ServiceKind::Futimes, {UINT64_MAX, 1}, 14);
  error(ServiceKind::Futimes, {UINT64_MAX, input()}, 9);
  refused(ServiceKind::Futimes, {UINT64_MAX, 0}, diagnostic::TimeOfDayMissing);
  error(ServiceKind::Utimes, {path("/missing"), 1}, 2);
  error(ServiceKind::Utimes, {path("/missing"), 0}, 2);
  error(route(), args(FD, 1), 14);
  const auto Partial = Base + Page * 4 - 16;
  refused(route(), args(FD, Partial), diagnostic::FileTimestampInput);
  refused(ServiceKind::Futimes, {UINT64_MAX, Partial},
          diagnostic::FileTimestampInput);
  error(ServiceKind::Utimes, {path("/missing"), Partial}, 2);
  EXPECT_EQ(status(FD), Before);
}
TEST_P(DarwinTimestampMutationTest, OwnershipRequiredEvenForEqualExplicitTime) {
  Options->Metadata["/data"].UID = 700;
  Options->Metadata["/data"].Mode = 0100666;
  reset();
  const auto FD = open();
  const auto Before = status(FD);
  error(route(), args(FD, input(1, 0, 3, 0)), 1);
  EXPECT_EQ(status(FD), Before);
  Time.emplace().TimeOfDay = DarwinTimeOfDay{21, 22};
  EXPECT_EQ(ok(route(), args(FD, 0)), 0u);
  EXPECT_EQ(status(FD), changed(Before, 21, 22000, 21, 22000));
}
TEST_P(DarwinTimestampMutationTest,
       OwnerIgnoresWriteBitsAndNonownerUsesKnownGroups) {
  Options->Metadata["/data"].Mode = 0100000;
  reset();
  auto FD = open();
  Time.emplace().TimeOfDay = DarwinTimeOfDay{21, 22};
  EXPECT_EQ(ok(route(), args(FD, 0)), 0u);
  Options->Metadata["/data"].UID = 700;
  reset();
  FD = open();
  auto Before = status(FD);
  error(route(), args(FD, 0), 13);
  EXPECT_EQ(status(FD), Before);
  Options->Metadata["/data"].Mode = 0100020;
  Options->Metadata["/data"].GID = 40;
  Credentials->GroupAccessList = {20};
  reset();
  FD = open();
  Before = status(FD);
  refused(route(), args(FD, 0), diagnostic::FileOrdinaryAuthorizationGroups);
  EXPECT_EQ(status(FD), Before);
  Credentials->GroupMembershipUID = 4294967195u;
  reset();
  FD = open();
  error(route(), args(FD, 0), 13);
  Credentials->GroupMembershipUID.reset();
  Credentials->GroupAccessList = {20, 40};
  reset();
  FD = open();
  EXPECT_EQ(ok(route(), args(FD, 0)), 0u);
}
TEST_P(DarwinTimestampMutationTest,
       MissingSecurityAndRootNeverGuessPermissions) {
  Credentials.reset();
  reset();
  const auto FD = open();
  refused(ServiceKind::Futimes, {FD, input()},
          diagnostic::FileOrdinaryAuthorizationCredentials);
  Credentials = darwin_test::modeCredentials();
  Credentials->EffectiveUID = 0;
  reset();
  const auto RootFD = open();
  refused(ServiceKind::Futimes, {RootFD, input()},
          diagnostic::FileOrdinaryAuthorizationSubject);
  Credentials = darwin_test::modeCredentials();
  Options->TimestampMutations->Objects.clear();
  Options->Metadata["/data"].Flags = 1;
  reset();
  const auto FlagFD = open();
  refused(route(), args(FlagFD, input()),
          diagnostic::FileTimestampSecurityAttributes);
}
TEST_P(DarwinTimestampMutationTest,
       EnvironmentGrantAndOtherAuthoritiesAreIndependent) {
  Options->TimestampMutations.reset();
  Options->ModeMutations.emplace().Objects["/data"] = {71, 72};
  Options->OwnershipMutations.emplace().Objects["/data"] = {81, 82};
  reset();
  auto FD = open();
  refused(route(), args(FD, input()), diagnostic::FileTimestampEnvironment);
  Options->TimestampMutations.emplace();
  reset();
  FD = open();
  const auto Before = status(FD);
  refused(route(), args(FD, input()), diagnostic::FileTimestampProvider);
  EXPECT_EQ(status(FD), Before);
  Options->ModeMutations.reset();
  Options->OwnershipMutations.reset();
  Options->TimestampMutations->Objects["/data"] = {111, 112};
  reset();
  FD = open();
  EXPECT_EQ(ok(route(), args(FD, input())), 0u);
  refused(ServiceKind::Fchmod, {FD, 0600}, diagnostic::FileModeEnvironment);
  refused(ServiceKind::Fchown, {FD, 501, 20},
          diagnostic::FileOwnershipEnvironment);
  refused(ServiceKind::Fsync, {FD}, diagnostic::FileSynchronizationEnvironment);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0u);
}
TEST_P(DarwinTimestampMutationTest, SymlinkLookupAndKindsStayDistinct) {
  Options->SymbolicLinks["/alias"] = {'/', 'd', 'a', 't', 'a'};
  reset();
  const auto FD = open();
  EXPECT_EQ(ok(ServiceKind::Utimes, {path("/alias"), input()}), 0u);
  EXPECT_EQ(status(FD),
            changed(darwin_test::timestampRecord(), 11, 12000, 13, 14000));
  refused(ServiceKind::Futimes, {0, input()}, diagnostic::FileTimestampKind);
  const auto Dir = open("/");
  refused(ServiceKind::Futimes, {Dir, input()},
          diagnostic::FileTimestampProvider);
  refused(ServiceKind::Utimes, {path("/"), input()},
          diagnostic::FileTimestampProvider);
}
TEST_P(DarwinTimestampMutationTest,
       GrantsFollowHardlinksRenameAndHeldZeroName) {
  policies();
  const auto A = open(), D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Link, {path("/data"), path("/alias", Base + 128)}),
            0u);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  const auto B = open("/moved");
  auto Before = status(A);
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
  auto Wanted = changed(Before, 11, 12000, 13, 14000);
  EXPECT_EQ(status(B), Wanted);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  Before = status(A);
  EXPECT_EQ(ok(ServiceKind::Futimes, {A, input(31, 32, 33, 34)}), 0u);
  EXPECT_EQ(status(D), changed(Before, 31, 32000, 33, 34000));
  const auto New = open("/data", 0x202);
  refused(ServiceKind::Futimes, {New, input()},
          diagnostic::FileTimestampProvider);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Futimes, {A, input()}, 9);
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
}
TEST_P(DarwinTimestampMutationTest,
       CreatedFlagPublishesOnlySuccessfulRegularGrants) {
  policies(true);
  const auto Initial = open();
  EXPECT_EQ(ok(ServiceKind::Futimes, {Initial, input()}), 0u);
  refused(ServiceKind::Open, {path("/failed"), 0x80000000 | 0x202, 0600},
          diagnostic::FileOpenFlags);
  const auto New = open("/failed", 0x202);
  const auto Before = status(New);
  EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 16), 100u);
  EXPECT_EQ(ok(ServiceKind::Futimes, {New, input()}), 0u);
  EXPECT_EQ(status(New),
            changed(Before, 11, 12000, 13, 14000, 13, 14000, 101, 102));
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  const auto Dir = open("/dir");
  refused(ServiceKind::Futimes, {Dir, input()},
          diagnostic::FileTimestampProvider);
  EXPECT_EQ(
      ok(ServiceKind::Symlink, {path("/failed"), path("/link", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Utimes, {path("/link"), input()}), 0u);
}
TEST_P(DarwinTimestampMutationTest, InvalidatedMetadataAndParentNeverRecover) {
  Options->WritableFiles.insert("/data");
  reset();
  const auto FD = open("/data", 2);
  path("Z", Base + 1024);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 1024, 1, 0}), 1u);
  refused(ServiceKind::Futimes, {FD, input()}, diagnostic::FileMutatedMetadata);
  Options = darwin_test::timestampOptions();
  Options->MutableDirectories.insert("/");
  reset();
  const auto A = open();
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/data"), path("/moved", Base + 128)}), 0u);
  refused(ServiceKind::Utimes, {path("/moved"), input()},
          diagnostic::DirectoryMutated);
  refused(ServiceKind::Futimes, {A, input()}, diagnostic::FileMutatedMetadata);
}
TEST_P(DarwinTimestampMutationTest,
       RealMappingRetainsBytesWithoutRevivingClosedFD) {
  policies();
  const auto FD = open();
  auto Source = Files->mappingSource(FD);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Source));
  ProcessOptions VMOptions;
  VMOptions.MemoryLimit = Page * 16;
  VMOptions.StackSize = Page;
  MemoryLayout Layout{Page, Base + Page * 8, {}};
  DarwinMemory VM(*Space, Layout, VMOptions);
  auto M = VM.handle(ServiceKind::Mmap, {0, 0, {0, Page, 1, 2, FD, 0}, {}},
                     *Files, Result);
  ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
  ASSERT_TRUE(*M);
  ASSERT_FALSE((**M).Error);
  const auto Address = (**M).Value;
  const auto Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(ok(ServiceKind::Futimes, {FD, input()}), 0u);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Futimes, {FD, input()}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Futimes, {FD, input()}, 9);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Address, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  auto U = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}}, *Files,
                     Result);
  ASSERT_TRUE(bool(U)) << llvm::toString(U.takeError());
  ASSERT_TRUE(*U);
  EXPECT_FALSE((**U).Error);
}
TEST(DarwinTimestampMutationOptions, ReferencesShareCatalogueByteBudget) {
  auto O = darwin_test::timestampOptions();
  O.Files["/data"].resize(darwin_file_limits::Bytes);
  O.Metadata["/data"].Size = O.Files["/data"].size();
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_P(DarwinTimestampMutationTest,
       DirtyBytesLocksAndAttributesStayIndependent) {
  Options->WritableFiles.insert("/data");
  Options->MutationPolicies["/data"] = {512, {101, 102}};
  Options->AdvisoryLocks.emplace().Objects.insert("/data");
  Options->ExtendedAttributes["/data"] = {{"user.test", {'v'}}};
  reset();
  const auto A = open("/data", 2), B = open("/data", 2);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  error(ServiceKind::Flock, {B, 2 | 4}, 35);
  EXPECT_EQ(ok(route(), args(A, input())), 0u);
  EXPECT_EQ(ok(ServiceKind::FgetXattr,
               {A, path("user.test", Base + 128), Base + 1024, 1}),
            1u);
  error(ServiceKind::Flock, {B, 2 | 4}, 35);
  path("Z", Base + 1024);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 1024, 1, 0}), 1u);
  EXPECT_EQ(ok(route(), args(A, input())), 0u);
  EXPECT_EQ(status(A),
            changed(darwin_test::timestampRecord(), 11, 12000, 13, 14000));
  EXPECT_EQ(ok(ServiceKind::Pread, {B, Base + 1024, 6, 0}), 6u);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Base + 1024, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'Z', 'b', 0, 255, 'e', 'f'}));
  error(ServiceKind::Flock, {B, 2 | 4}, 35);
  refused(ServiceKind::FgetXattr, {A, path("user.test", Base + 128), 0, 0},
          diagnostic::ExtendedAttributeUnknown);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 2 | 4}), 0u);
}

TEST(DarwinTimestampMutationOptions,
     DirectoryAdmissionKeepsAliasesAndCreationIndependent) {
  auto O = darwin_test::directoryTimestampOptions();
  EXPECT_FALSE(O.TimestampMutations->CreatedDirectoryObjects);
  O.Metadata["/dir"].LinkCount = 7;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/alias");
  O.Metadata["/alias"] = O.Metadata["/dir"];
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Metadata["/alias"].Device = 8;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.TimestampMutations->Objects["/"] = {1, 2};
  O.MutableDirectories.clear();
  O.MovableDirectories.clear();
  O.DirectoryMutationPolicies.clear();
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.TimestampMutations->CreatedDirectoryObjects = true;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.CreationPolicy =
      darwin_test::directoryTimestampOptions(true).CreationPolicy;
  O.MutableDirectories.insert("/");
  O.InitialUmask = 022;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.CreationPolicy->Namespace.reset();
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.TimestampMutations->CreatedDirectoryObjects = false;
  EXPECT_FALSE(bool(validateFileOptions(O)));
}
TEST_P(DarwinTimestampMutationTest,
       DirectoryCompleteRecordsAndNullSampleStayIndependent) {
  Options = darwin_test::directoryTimestampOptions();
  reset();
  const auto A = open("/dir"), D = ok(ServiceKind::Dup, {A}), B = open("/dir");
  const auto Before = darwin_test::directoryTimestampRecord();
  EXPECT_EQ(status(A), Before);
  const auto P = input();
  std::array<uint8_t, 32> Original, After;
  llvm::cantFail(Space->read(P, Original));
  EXPECT_EQ(
      ok(route(), {FDRoute ? (0xffffffff00000000ULL | D) : path("/dir"), P}),
      0u);
  auto Wanted = changed(Before, 11, 12000, 13, 14000);
  EXPECT_EQ(status(A), Wanted);
  EXPECT_EQ(status(B), Wanted);
  EXPECT_EQ(status(D), Wanted);
  llvm::cantFail(Space->read(P, After));
  EXPECT_EQ(Original, After);
  refused(route(), {FDRoute ? D : path("/dir"), 0},
          diagnostic::TimeOfDayMissing);
  EXPECT_EQ(status(A), Wanted);
  Time.emplace().TimeOfDay = DarwinTimeOfDay{21, 22};
  EXPECT_EQ(ok(route(), {FDRoute ? D : path("/dir"), 0}), 0u);
  Wanted = changed(Before, 21, 22000, 21, 22000);
  EXPECT_EQ(status(A), Wanted);
  error(ServiceKind::Futimes, {UINT64_MAX, 1}, 14);
  error(ServiceKind::Futimes, {UINT64_MAX, P}, 9);
  error(ServiceKind::Utimes, {path("/missing"), 1}, 2);
  EXPECT_EQ(status(D), Wanted);
  refused(ServiceKind::Futimes, {open("/data"), P},
          diagnostic::FileTimestampProvider);
  refused(ServiceKind::Fchmod, {D, 0750}, diagnostic::FileModeEnvironment);
  refused(ServiceKind::Fchown, {D, 501, 20},
          diagnostic::FileOwnershipEnvironment);
  EXPECT_EQ(status(D), Wanted);
  // Earlier nonnegative mtime updates only the declared birth observation.
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, input(1, 2, 2, 3)}), 0u);
  EXPECT_EQ(status(A), changed(Before, 1, 2000, 2, 3000, 2, 3000));
}
TEST_P(DarwinTimestampMutationTest,
       CreatedDirectoryAndRegularTimestampFlagsAreSeparate) {
  for (bool Regular : {false, true})
    for (bool Directory : {false, true}) {
      Options = darwin_test::timestampOptions();
      policies(Regular);
      Options->TimestampMutations->CreatedDirectoryObjects = Directory;
      reset();
      EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/child"), 0700}), 0u);
      const auto D = open("/child");
      const auto Before = status(D);
      if (Directory) {
        EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
        EXPECT_EQ(status(D),
                  changed(Before, 11, 12000, 13, 14000, 13, 14000, 101, 102));
      } else {
        refused(ServiceKind::Futimes, {D, input()},
                diagnostic::FileTimestampProvider);
        EXPECT_EQ(status(D), Before);
      }
      const auto F = open("/new", 0x202);
      const auto NewBefore = status(F);
      if (Regular) {
        EXPECT_EQ(ok(ServiceKind::Futimes, {F, input()}), 0u);
        EXPECT_EQ(status(F), changed(NewBefore, 11, 12000, 13, 14000, 13, 14000,
                                     101, 102));
      } else {
        refused(ServiceKind::Futimes, {F, input()},
                diagnostic::FileTimestampProvider);
        EXPECT_EQ(status(F), NewBefore);
      }
      const auto Root = open("/");
      const auto RootBefore = status(Root);
      refused(ServiceKind::Futimes, {Root, input()},
              diagnostic::FileTimestampProvider);
      EXPECT_EQ(status(Root), RootBefore);
    }
}
TEST_P(DarwinTimestampMutationTest,
       DirectoryNamespaceNeverBorrowsTimestampAuthority) {
  for (bool NamespaceTime : {false, true})
    for (bool TimestampFirst : {false, true}) {
      Options = darwin_test::directoryTimestampOptions();
      Options->MutableDirectories.insert("/dir");
      if (!NamespaceTime)
        Options->DirectoryMutationPolicies.erase("/dir");
      reset();
      const auto D = open("/dir");
      auto Expected = darwin_test::directoryTimestampRecord();
      if (TimestampFirst) {
        EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
        Expected = changed(Expected, 11, 12000, 13, 14000);
      }
      EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700}), 0u);
      if (!NamespaceTime) {
        Record Buffer;
        Buffer.fill(0xa5);
        llvm::cantFail(Space->write(Base + Page, Buffer));
        refused(ServiceKind::Fstat64, {D, Base + Page + 8},
                diagnostic::DirectoryMutated);
        refused(ServiceKind::Futimes, {D, input()},
                diagnostic::DirectoryMutated);
        Record After;
        llvm::cantFail(Space->read(Base + Page, After));
        EXPECT_EQ(After, Buffer);
        continue;
      }
      llvm::support::endian::write16le(Expected.data() + 14, 3);
      llvm::support::endian::write64le(Expected.data() + 104, 48);
      time(Expected, 48, 81, 82);
      time(Expected, 64, 81, 82);
      EXPECT_EQ(status(D), Expected);
      EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
      EXPECT_EQ(status(D), changed(Expected, 11, 12000, 13, 14000));
    }
}
TEST_P(DarwinTimestampMutationTest,
       DirectoryMovesAndExchangesKeepActualObjectGrants) {
  for (bool Swap : {false, true})
    for (bool NamespaceTime : {false, true})
      for (bool First : {false, true}) {
        Options = darwin_test::directoryTimestampOptions();
        Options->Directories = {"/", "/a", "/b"};
        Options->Metadata.erase("/dir");
        Options->DirectoryMutationPolicies.erase("/dir");
        Options->MovableDirectories.erase("/dir");
        Options->Metadata["/a"] = darwin_test::modeMetadata(0040700, 3);
        Options->Metadata["/b"] = darwin_test::modeMetadata(0040700, 4);
        Options->MovableDirectories.insert("/a");
        Options->ExchangeableDirectories = {"/a", "/b"};
        Options->SwapRenameDirectories.insert("/");
        Options->TimestampMutations->Objects.clear();
        Options->TimestampMutations->Objects["/a"] = {111, 112};
        if (NamespaceTime) {
          Options->DirectoryMutationPolicies["/a"] = {16, {81, 82}};
          Options->DirectoryMutationPolicies["/b"] = {16, {91, 92}};
        }
        reset();
        const auto A = open("/a"), D = ok(ServiceKind::Dup, {A}),
                   B = open("/b");
        auto Expected = status(A), Other = status(B);
        if (First) {
          EXPECT_EQ(ok(ServiceKind::Futimes, {A, input()}), 0u);
          Expected = changed(Expected, 11, 12000, 13, 14000);
        }
        const auto From = path("/a"),
                   To = path(Swap ? "/b" : "/moved", Base + 128);
        if (Swap)
          EXPECT_EQ(
              ok(ServiceKind::RenameAtX, {UINT64_MAX, From, UINT64_MAX, To, 2}),
              0u);
        else
          EXPECT_EQ(ok(ServiceKind::Rename, {From, To}), 0u);
        if (!NamespaceTime) {
          refused(ServiceKind::Fstat64, {D, Base + Page + 8},
                  diagnostic::DirectoryMutated);
          refused(ServiceKind::Futimes, {D, input()},
                  diagnostic::DirectoryMutated);
          continue;
        }
        time(Expected, 64, 81, 82);
        EXPECT_EQ(status(D), Expected);
        EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
        Expected = changed(Expected, 11, 12000, 13, 14000);
        EXPECT_EQ(status(open(Swap ? "/b" : "/moved")), Expected);
        EXPECT_EQ(status(A), Expected);
        if (Swap)
          time(Other, 64, 91, 92);
        EXPECT_EQ(status(B), Other);
        refused(ServiceKind::Futimes, {B, input()},
                diagnostic::FileTimestampProvider);
        EXPECT_EQ(status(B), Other);
      }
}
TEST_P(DarwinTimestampMutationTest,
       HeldRemovedDirectoryAndReplacementNeverShareTimestampGrants) {
  Options = darwin_test::directoryTimestampOptions();
  Options->RemovableDirectories.insert("/dir");
  Options->CreationPolicy =
      darwin_test::directoryTimestampOptions(true).CreationPolicy;
  Options->InitialUmask = 022;
  reset();
  const auto A = open("/dir"), D = ok(ServiceKind::Dup, {A});
  auto Expected = darwin_test::directoryTimestampRecord();
  EXPECT_EQ(ok(ServiceKind::Rmdir, {path("/dir")}), 0u);
  time(Expected, 64, 81, 82);
  EXPECT_EQ(status(D), Expected);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  const auto New = open("/dir");
  const auto Before = status(New);
  refused(ServiceKind::Futimes, {New, input()},
          diagnostic::FileTimestampProvider);
  EXPECT_EQ(status(New), Before);
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
  EXPECT_EQ(status(A), changed(Expected, 11, 12000, 13, 14000));
  EXPECT_EQ(status(New), Before);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Futimes, {A, input()}, 9);
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
}
TEST_P(DarwinTimestampMutationTest,
       DirectoryNonownerExplicitAndCurrentAuthorizationRemainDistinct) {
  Options = darwin_test::directoryTimestampOptions();
  Options->Metadata["/"].Mode = 0040755;
  Options->Metadata["/dir"].UID = 600;
  Options->Metadata["/dir"].Mode = 0040666;
  reset();
  const auto D = open("/dir");
  const auto Before = status(D);
  error(route(), {FDRoute ? D : path("/dir"), input(1, 0, 3, 0)}, 1);
  EXPECT_EQ(status(D), Before);
  Time.emplace().TimeOfDay = DarwinTimeOfDay{21, 22};
  EXPECT_EQ(ok(route(), {FDRoute ? D : path("/dir"), 0}), 0u);
  EXPECT_EQ(status(D), changed(Before, 21, 22000, 21, 22000));
  Options->Metadata["/dir"].Mode = 0040644;
  reset();
  const auto Denied = open("/dir");
  const auto DeniedBefore = status(Denied);
  error(route(), {FDRoute ? Denied : path("/dir"), 0}, 13);
  EXPECT_EQ(status(Denied), DeniedBefore);
}
TEST_P(DarwinTimestampMutationTest,
       DirectoryEnumerationAndChildrenKeepTheirOwnState) {
  Options = darwin_test::directoryTimestampOptions();
  Options->Files["/dir/leaf"] = {'x'};
  Options->Metadata["/dir/leaf"] = darwin_test::modeMetadata(0100600, 4, 1);
  Options->Directories.insert("/dir/sub");
  Options->Metadata["/dir/sub"] = darwin_test::modeMetadata(0040700, 5);
  Options->Metadata["/dir"].Size = 96;
  Options->DirectoryContents["/dir"] = {{{".", 3, 4, 11, 0},
                                         {"..", 1, 4, 22, 0},
                                         {"leaf", 4, 8, 33, 0},
                                         {"sub", 5, 4, 44, 0}},
                                        1};
  reset();
  const auto A = open("/dir"), D = ok(ServiceKind::Dup, {A}), B = open("/dir");
  const auto Before = status(A);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {A, Base + Page * 2, 32, Base + 512}),
      32u);
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, input()}), 0u);
  EXPECT_EQ(status(B), changed(Before, 11, 12000, 13, 14000));
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 11u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Utimes, {path("."), input()}), 0u);
  for (auto F : {open("leaf"), open("sub")}) {
    const auto Child = status(F);
    refused(ServiceKind::Futimes, {F, input()},
            diagnostic::FileTimestampProvider);
    EXPECT_EQ(status(F), Child);
  }
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {D, Base + Page * 2, 32, Base + 512}),
      32u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 22u);
  refused(ServiceKind::Mkdir, {path("/dir/new"), 0700},
          diagnostic::DirectoryNotMutable);
  EXPECT_EQ(status(A), changed(Before, 11, 12000, 13, 14000));
}

INSTANTIATE_TEST_SUITE_P(PageAndRoute, DarwinTimestampMutationTest,
                         testing::Combine(testing::Values(uint64_t(4096),
                                                          uint64_t(16384)),
                                          testing::Bool()));
} // namespace
} // namespace neverd::emulation::darwin_model
