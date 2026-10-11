//===- DarwinOwnershipTests.cpp - Current ordinary ownership contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinOwnershipTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinOwnershipTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only ownership"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::ownershipOptions();
    Credentials = darwin_test::modeCredentials();
    reset();
  }
  void reset() {
    if (Options)
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(
        *Space, Options, process_defaults::Output, 777, Credentials);
  }
  uint64_t path(llvm::StringRef Text, uint64_t Address = Base) {
    const auto Bytes = Text.str() + '\0';
    llvm::cantFail(Space->write(
        Address,
        llvm::ArrayRef<uint8_t>((const uint8_t *)Bytes.data(), Bytes.size())));
    return Address;
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result =
        ProcessResult{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                      ExecutionBackendKind::Unicorn, "memory-only ownership"};
    auto Out = Files->handle(K, {0, 0, A, {}}, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  uint64_t ok(ServiceKind K, std::array<uint64_t, 6> A) {
    auto Out = invoke(K, A);
    EXPECT_TRUE(Out) << Result.Diagnostic;
    EXPECT_FALSE(Out && Out->Error);
    return Out ? Out->Value : UINT64_MAX;
  }
  void error(ServiceKind K, std::array<uint64_t, 6> A, uint32_t Errno) {
    auto Out = invoke(K, A);
    ASSERT_TRUE(Out) << Result.Diagnostic;
    EXPECT_TRUE(Out->Error);
    EXPECT_EQ(Out->Value, Errno);
  }
  void refused(ServiceKind K, std::array<uint64_t, 6> A, const char *Reason) {
    EXPECT_FALSE(invoke(K, A));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Reason);
  }
  uint64_t open(llvm::StringRef Name = "/data", uint32_t Flags = 0) {
    return ok(ServiceKind::Open, {path(Name), Flags});
  }
  using Record = std::array<uint8_t, 160>;
  Record status(uint32_t FD) {
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
  Record changed(Record B, uint32_t UID = 501, uint32_t GID = 20,
                 int64_t Seconds = 81, int64_t Nanoseconds = 82) {
    // Literal offsets in guarded stat64; preserve every unrequested byte.
    llvm::support::endian::write32le(B.data() + 24, UID);
    llvm::support::endian::write32le(B.data() + 28, GID);
    llvm::support::endian::write64le(B.data() + 72, Seconds);
    llvm::support::endian::write64le(B.data() + 80, Nanoseconds);
    return B;
  }
  std::array<uint64_t, 6> operands(ServiceKind K, uint64_t FD, uint64_t UID,
                                   uint64_t GID,
                                   llvm::StringRef Name = "/data") {
    if (K == ServiceKind::Fchown)
      return {FD, UID, GID};
    if (K == ServiceKind::FchownAt)
      return {UINT64_MAX, path(Name), UID, GID, 0};
    return {path(Name), UID, GID};
  }
  static constexpr ServiceKind Routes[] = {
      ServiceKind::Chown, ServiceKind::Fchown, ServiceKind::FchownAt};
  void mutationPolicies() {
    Options->WritableFiles.insert("/data");
    Options->MutationPolicies["/data"] = {512, {101, 102}};
    Options->MutableDirectories.insert("/");
    Options->DirectoryMutationPolicies["/"] = {16, {91, 92}};
    Options->InitialUmask = 022;
    DarwinFileCreationPolicy P;
    P.FirstInode = 100;
    P.BlockSize = 512;
    P.Time = {41, 42};
    P.Mutation = {512, {101, 102}};
    Options->CreationPolicy = P;
    reset();
  }
};
TEST(DarwinOwnershipOptions,
     CompleteInitialOrdinaryUniqueGrantsAndQueryClosure) {
  const auto Original = darwin_test::ownershipOptions();
  for (unsigned I = 0; I != 9; ++I) {
    auto O = Original;
    switch (I) {
    case 0:
      O.OwnershipMutations->Objects["/missing"] = {1, 2};
      break;
    case 1:
      O.OwnershipMutations->Objects["/"] = {1, 2};
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
      O.OwnershipMutations->Objects["/data"].Nanoseconds = -1;
      break;
    case 7:
      O.OwnershipMutations->Objects["/data"].Nanoseconds = 1000000000;
      break;
    case 8:
      O.Files["/alias"] = O.Files["/data"];
      O.Metadata["/alias"] = O.Metadata["/data"];
      break;
    }
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << I;
    llvm::consumeError(std::move(E));
  }
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries}) {
    auto O = Original;
    O.Authorization = A;
    O.OwnershipMutations->Objects.clear();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST(DarwinOwnershipOptions, ReferencesShareTheInputByteBudget) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 6);
  O.Metadata["/a"] =
      darwin_test::modeMetadata(0100000, 1, O.Files["/a"].size());
  O.OwnershipMutations.emplace().Objects["/a"] = {1, 2};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files["/a"].pop_back();
  --O.Metadata["/a"].Size;
  O.ModeMutations.emplace().Objects["/a"] = {3, 4};
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_P(DarwinOwnershipTest, TypedCarriersSentinelsAndAtomicFullRecords) {
  for (auto K : Routes)
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      reset();
      const auto FD = open();
      const auto Before = status(FD);
      EXPECT_EQ(
          ok(K, operands(K, High | FD, High | UINT32_MAX, High | UINT32_MAX)),
          0u);
      EXPECT_EQ(status(FD), Before);
      EXPECT_EQ(ok(K, operands(K, High | FD, High | 501, High | UINT32_MAX)),
                0u);
      EXPECT_EQ(status(FD), changed(Before, 501, 31));
      EXPECT_EQ(ok(K, operands(K, High | FD, High | UINT32_MAX, High | 20)),
                0u);
      EXPECT_EQ(status(FD), changed(Before));
      EXPECT_EQ(ok(K, operands(K, High | FD, High | 501, High | 20)), 0u);
      EXPECT_EQ(status(FD), changed(Before));
      EXPECT_EQ(
          ok(K, operands(K, High | FD, High | UINT32_MAX, High | UINT32_MAX)),
          0u);
      EXPECT_EQ(status(FD), changed(Before));
    }
  EXPECT_EQ(Options->Metadata.at("/data").GID, 31u);
  EXPECT_EQ(Options->Metadata.at("/data").ChangeTime.Seconds, 5);
}
TEST_P(DarwinOwnershipTest, NonownerSameIDsHaveNoRightsButStillNeedProvider) {
  Options->Metadata["/data"].UID = 700;
  reset();
  const auto FD = open();
  auto Current = status(FD);
  for (auto K : Routes) {
    error(K, operands(K, FD, 1, UINT32_MAX), 1);
    error(K, operands(K, FD, 501, UINT32_MAX), 1);
    error(K, operands(K, FD, UINT32_MAX, 20), 1);
    EXPECT_EQ(status(FD), Current);
    EXPECT_EQ(ok(K, operands(K, FD, UINT32_MAX, UINT32_MAX)), 0u);
    EXPECT_EQ(status(FD), Current);
    EXPECT_EQ(ok(K, operands(K, FD, 700, 31)), 0u);
    Current = changed(Current, 700, 31);
    EXPECT_EQ(status(FD), Current);
  }
  Options->OwnershipMutations->Objects.clear();
  reset();
  const auto Ungranted = open();
  for (auto K : Routes) {
    error(K, operands(K, Ungranted, 501, UINT32_MAX), 1);
    refused(K, operands(K, Ungranted, 700, 31),
            diagnostic::FileOwnershipProvider);
    refused(K, operands(K, Ungranted, UINT32_MAX, UINT32_MAX),
            diagnostic::FileOwnershipProvider);
  }
}
TEST_P(DarwinOwnershipTest, TargetGroupsUseExplicitMembershipAndCurrentGID) {
  for (auto K : Routes) {
    Credentials = darwin_test::modeCredentials();
    reset();
    const auto FD = open();
    const auto Before = status(FD);
    refused(K, operands(K, FD, UINT32_MAX, 12),
            diagnostic::FileOrdinaryAuthorizationGroups);
    EXPECT_EQ(status(FD), Before);
    Credentials->GroupAccessList = {20, 12};
    reset();
    const auto Known = open();
    EXPECT_EQ(ok(K, operands(K, Known, UINT32_MAX, 12)), 0u);
    EXPECT_EQ(status(Known), changed(Before, 501, 12));
    EXPECT_EQ(ok(K, operands(K, Known, UINT32_MAX, 12)), 0u);
    refused(K, operands(K, Known, UINT32_MAX, 31),
            diagnostic::FileOrdinaryAuthorizationGroups);
    EXPECT_EQ(status(Known), changed(Before, 501, 12));
    Credentials->GroupMembershipUID = 4294967195u;
    reset();
    const auto Negative = open();
    error(K, operands(K, Negative, UINT32_MAX, 56), 1);
    EXPECT_EQ(status(Negative), Before);
    Credentials->GroupMembershipUID = 501;
    reset();
    refused(K, operands(K, open(), UINT32_MAX, 56),
            diagnostic::FileOrdinaryAuthorizationGroups);
  }
}
TEST_P(DarwinOwnershipTest,
       UIDDenialPrecedesGroupAndGroupPrecedesTakeOwnership) {
  Options->Metadata["/data"].UID = 700;
  reset();
  const auto FD = open();
  const auto Before = status(FD);
  for (auto K : Routes) {
    error(K, operands(K, FD, 1, 56), 1);
    error(K, operands(K, FD, 0, 56), 1);
    refused(K, operands(K, FD, 501, 56),
            diagnostic::FileOrdinaryAuthorizationGroups);
    error(K, operands(K, FD, 501, 20), 1);
    EXPECT_EQ(status(FD), Before);
  }
  Options->Metadata["/data"].UID = 0;
  reset();
  const auto RootOwned = open();
  for (auto K : Routes)
    EXPECT_EQ(ok(K, operands(K, RootOwned, 0, 31)), 0u);
}
TEST_P(DarwinOwnershipTest, EffectiveExplicitNonrootSubjectAndModeZero) {
  Credentials->RealUID = 0;
  Options->Metadata["/data"].Mode = 0100000;
  reset();
  EXPECT_EQ(ok(ServiceKind::Fchown, {open(), UINT32_MAX, 20}), 0u);
  Credentials.reset();
  reset();
  refused(ServiceKind::Fchown, {open(), 501, 31},
          diagnostic::FileOrdinaryAuthorizationCredentials);
  Credentials = darwin_test::modeCredentials();
  Credentials->EffectiveUID = 0;
  reset();
  refused(ServiceKind::Fchown, {open(), 501, 31},
          diagnostic::FileOrdinaryAuthorizationSubject);
}
TEST_P(DarwinOwnershipTest, UnsupportedCurrentSecurityAttributesStayUnknown) {
  Options->OwnershipMutations->Objects.clear();
  Options->Metadata["/data"].Flags = 1;
  reset();
  refused(ServiceKind::Fchown, {open(), UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipSecurityAttributes);
  Options->Metadata["/data"].Flags = 0;
  Options->Metadata["/data"].Mode |= 04000;
  reset();
  refused(ServiceKind::Fchown, {open(), 501, 31},
          diagnostic::FileOwnershipSecurityAttributes);
  Options = darwin_test::ownershipOptions();
  Options->Metadata["/"].Flags = 1;
  reset();
  refused(ServiceKind::Chown, {path("/data"), 501, 31},
          diagnostic::FileOwnershipSecurityAttributes);
}
TEST_P(DarwinOwnershipTest,
       AbsentEmptyAndMissingCatalogueKeepIndependentPreflight) {
  for (unsigned State = 0; State != 3; ++State) {
    Options = darwin_test::ownershipOptions();
    if (State == 0)
      Options->OwnershipMutations.reset();
    if (State == 1)
      Options->OwnershipMutations->Objects.clear();
    if (State == 2)
      Options.reset();
    reset();
    error(ServiceKind::Chown, {UINT64_MAX, 501, 31}, 14);
    error(ServiceKind::Fchown, {UINT64_MAX, 501, 31}, 9);
    error(ServiceKind::FchownAt, {UINT64_MAX, UINT64_MAX, 501, 31, 0x800}, 22);
    error(ServiceKind::FchownAt, {UINT64_MAX, path("data"), 501, 31, 0}, 9);
    error(ServiceKind::Chown, {path(""), 501, 31}, 2);
    error(ServiceKind::FchownAt, {uint32_t(-2), path(""), 501, 31, 0}, 2);
    if (State == 1) {
      error(ServiceKind::Chown, {path("/missing"), 501, 31}, 2);
      refused(ServiceKind::FchownAt, {UINT64_MAX, path("/data"), 501, 31, 0},
              diagnostic::FileOwnershipProvider);
    } else
      refused(ServiceKind::Chown, {path("/missing"), 501, 31},
              diagnostic::FileOwnershipEnvironment);
    if (Options)
      refused(ServiceKind::Fchown, {open(), 501, 31},
              State == 1 ? diagnostic::FileOwnershipProvider
                         : diagnostic::FileOwnershipEnvironment);
  }
}
TEST_P(DarwinOwnershipTest, FlagsPrefixAndReadonlyGuestPathPrecedeAuthority) {
  const auto FD = open();
  const auto Before = status(FD);
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    for (uint32_t Flag : {0x800u, 0x820u, 0x4000u})
      error(ServiceKind::FchownAt,
            {UINT64_MAX, UINT64_MAX, 501, 20, High | Flag}, 22);
    error(ServiceKind::FchownAt, {UINT64_MAX, UINT64_MAX, 501, 20, High}, 14);
    error(ServiceKind::FchownAt, {FD, path("data"), 501, 20, High}, 20);
    EXPECT_EQ(ok(ServiceKind::FchownAt,
                 {UINT64_MAX, path("/data"), 501, 20, High | 0x20}),
              0u);
  }
  EXPECT_EQ(status(FD), changed(Before));
  llvm::cantFail(Space->protect(Base, Page, Read | UserAccessible));
  EXPECT_EQ(ok(ServiceKind::Chown, {Base, 501, 20}), 0u);
  EXPECT_EQ(status(FD), changed(Before));
}
TEST_P(DarwinOwnershipTest,
       SearchUsesActualEffectiveParentBeforeMissingOrDots) {
  Options->Directories.insert("/locked");
  Options->Metadata["/locked"] = darwin_test::modeMetadata(0040000, 3);
  reset();
  for (llvm::StringRef Name :
       {"/locked/missing", "/locked/../data", "/locked/./missing"})
    error(ServiceKind::Chown, {path(Name), 501, 20}, 13);
  error(ServiceKind::FchownAt, {open("/locked"), path("missing"), 501, 20, 0},
        13);
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/../.././data"), 501, 20}), 0u);
  Options->Metadata["/locked"].Mode = 0040700;
  reset();
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/locked/../data"), 501, 20}), 0u);
  Options->Metadata.erase("/locked");
  reset();
  refused(ServiceKind::Chown, {path("/locked/missing"), 501, 20},
          diagnostic::FileMetadata);
  Options = darwin_test::ownershipOptions();
  Options->OwnershipMutations->Objects.clear();
  Options->Metadata["/"].Mode = 0040000;
  reset();
  error(ServiceKind::Chown, {path("/missing"), 501, 20}, 13);
}
TEST_P(DarwinOwnershipTest, SearchRetainsWholeMaskMembershipKnowledge) {
  auto &M = Options->Metadata["/"];
  M.Mode = 0040010;
  M.UID = 700;
  M.GID = 40;
  reset();
  refused(ServiceKind::Chown, {path("/data"), 501, 20},
          diagnostic::FileOrdinaryAuthorizationGroups);
  Credentials->GroupAccessList = {20, 40};
  reset();
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/data"), 501, 20}), 0u);
  Credentials->GroupAccessList = {20};
  reset();
  refused(ServiceKind::Chown, {path("/data"), 501, 20},
          diagnostic::FileOrdinaryAuthorizationGroups);
  Credentials->GroupMembershipUID = 4294967195u;
  reset();
  error(ServiceKind::Chown, {path("/data"), 501, 20}, 13);
  M.Mode = 0040011;
  Credentials->GroupAccessList.reset();
  Credentials->GroupMembershipUID.reset();
  reset();
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/data"), 501, 20}), 0u);
}
TEST_P(DarwinOwnershipTest,
       InvalidatedDirectoryAndObjectNeverRecoverAuthority) {
  Options->MutableDirectories.insert("/");
  reset();
  const auto FD = open();
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/data"), path("/renamed", Base + 128)}),
      0u);
  refused(ServiceKind::Chown, {path("/renamed"), 501, 20},
          diagnostic::DirectoryMutated);
  refused(ServiceKind::Chown, {path("/missing"), 501, 20},
          diagnostic::DirectoryMutated);
  refused(ServiceKind::Fchown, {FD, 501, 20}, diagnostic::FileMutatedMetadata);
}
TEST_P(DarwinOwnershipTest, LinksFollowActualSearchAndFinalKindsStayUnknown) {
  Options->SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
  Options->SymbolicLinks["/via"] = {'/'};
  reset();
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/alias"), 501, 20}), 0u);
  refused(ServiceKind::FchownAt, {UINT64_MAX, path("/alias"), 501, 20, 0x20},
          diagnostic::FileOwnershipTarget);
  EXPECT_EQ(
      ok(ServiceKind::FchownAt, {UINT64_MAX, path("/via/data"), 501, 20, 0x20}),
      0u);
  refused(ServiceKind::Chown, {path("/"), 501, 20},
          diagnostic::FileOwnershipTarget);
  refused(ServiceKind::Fchown, {open("/"), 501, 20},
          diagnostic::FileOwnershipTarget);
  refused(ServiceKind::Fchown, {1, 501, 20}, diagnostic::FileOwnershipTarget);
}
TEST_P(DarwinOwnershipTest, ReadonlyDupAndSeparateOpensShareMetadataOnly) {
  const auto FD = open(), Copy = ok(ServiceKind::Dup, {FD}), Separate = open();
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {Copy, 501, 20}), 0u);
  for (auto F : {FD, Copy, Separate})
    EXPECT_EQ(status(F), changed(Before));
  EXPECT_EQ(ok(ServiceKind::Lseek, {Copy, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Separate, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {Separate, Base + Page * 2, 6}), 6u);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Base + Page * 2, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Fchown, {FD, 501, 20}, 9);
  EXPECT_EQ(ok(ServiceKind::Fchown, {Copy, UINT32_MAX, UINT32_MAX}), 0u);
  EXPECT_EQ(status(Separate), changed(Before));
}
TEST_P(DarwinOwnershipTest, ModeDataAndOwnershipKeepSeparatePoliciesAndTimes) {
  Options->ModeMutations.emplace().Objects["/data"] = {71, 72};
  mutationPolicies();
  const auto FD = open("/data", 2);
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 20}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0000}), 0u);
  auto Current = changed(Before, 501, 20, 71, 72);
  llvm::support::endian::write16le(Current.data() + 12, 0100000);
  EXPECT_EQ(status(FD), Current);
  llvm::cantFail(Space->writeInteger(Base + Page * 2, 'x', 1));
  EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + Page * 2, 1, 0}), 1u);
  Current = changed(Current, 501, 20, 101, 102);
  llvm::support::endian::write64le(Current.data() + 56, 101);
  llvm::support::endian::write64le(Current.data() + 64, 102);
  EXPECT_EQ(status(FD), Current);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, UINT32_MAX, UINT32_MAX}), 0u);
  EXPECT_EQ(status(FD), Current);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 20}), 0u);
  EXPECT_EQ(status(FD), changed(Current));
  Options->MutationPolicies.clear();
  reset();
  const auto Unknown = open("/data", 2);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {Unknown, Base + Page * 2, 1, 0}), 1u);
  refused(ServiceKind::Fchown, {Unknown, 501, 20},
          diagnostic::FileMutatedMetadata);
  Options = darwin_test::ownershipOptions();
  Options->ModeMutations.emplace();
  reset();
  EXPECT_EQ(ok(ServiceKind::Fchown, {open(), 501, 20}), 0u);
  refused(ServiceKind::Fchmod, {3, 0600}, diagnostic::FileModeProvider);
  Options->ModeMutations->Objects["/data"] = {71, 72};
  Options->OwnershipMutations->Objects.clear();
  reset();
  EXPECT_EQ(ok(ServiceKind::Fchmod, {open(), 0600}), 0u);
  refused(ServiceKind::Fchown, {3, 501, 31}, diagnostic::FileOwnershipProvider);
}
TEST_P(DarwinOwnershipTest, XattrMutationCannotRestoreCompleteMetadata) {
  Options->ExtendedAttributes["/data"] = {{"user.owner", {5}}};
  Options->MutableExtendedAttributes.insert("/data");
  reset();
  const auto FD = open();
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 20}), 0u);
  EXPECT_EQ(ok(ServiceKind::FgetXattr,
               {FD, path("user.owner", Base + 128), Base + Page * 2, 8}),
            1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page * 2, 1)), 5u);
  EXPECT_EQ(status(FD), changed(Before));
  llvm::cantFail(Space->writeInteger(Base + Page * 2, 6, 1));
  EXPECT_EQ(ok(ServiceKind::FsetXattr, {FD, Base + 128, Base + Page * 2, 1}),
            0u);
  refused(ServiceKind::Fchown, {FD, UINT32_MAX, UINT32_MAX},
          diagnostic::FileMutatedMetadata);
}
TEST_P(DarwinOwnershipTest, HardlinksRenameHeldUnlinkAndNameReuseRetainGrants) {
  mutationPolicies();
  const auto FD = open();
  EXPECT_EQ(ok(ServiceKind::Link, {path("/data"), path("/alias", Base + 128)}),
            0u);
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/alias"), 501, 20}), 0u);
  const auto Linked = status(FD);
  EXPECT_EQ(llvm::support::endian::read16le(Linked.data() + 14), 2u);
  EXPECT_EQ(status(open("/alias")), Linked);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Chown, {path("/moved"), 501, 20}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Unlinked = status(FD);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, UINT32_MAX, UINT32_MAX}), 0u);
  EXPECT_EQ(status(FD), Unlinked);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 20}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(FD).data() + 14), 0u);
  const auto New = open("/data", 0x202);
  const auto Created = status(New);
  // Creation retains its independently declared legacy observation. Changing
  // that UID to the effective caller still asks for TAKE_OWNERSHIP first.
  EXPECT_EQ(llvm::support::endian::read32le(Created.data() + 24), 777u);
  error(ServiceKind::Fchown, {New, 501, 20}, 1);
  refused(ServiceKind::Fchown, {New, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipProvider);
  EXPECT_EQ(status(New), Created);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 20}), 0u);
  EXPECT_NE(llvm::support::endian::read64le(status(New).data() + 16),
            llvm::support::endian::read64le(status(FD).data() + 16));
}
TEST_P(DarwinOwnershipTest, MappedObjectKeepsBytesIdentityAndVMLifetime) {
  mutationPolicies();
  const auto FD = open();
  auto Before = Files->mappingSource(FD);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Before));
  const auto &Source = std::get<DarwinFiles::Mapping>(Before);
  ProcessOptions VMOptions;
  VMOptions.MemoryLimit = Page * 16;
  VMOptions.StackSize = Page;
  MemoryLayout Layout{Page, Base + Page * 8, {}};
  DarwinMemory VM(*Space, Layout, VMOptions);
  auto Mapped = VM.handle(ServiceKind::Mmap, {0, 0, {0, Page, 1, 2, FD, 0}, {}},
                          *Files, Result);
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  ASSERT_TRUE(*Mapped) << Result.Diagnostic;
  ASSERT_FALSE((**Mapped).Error);
  const auto Address = (**Mapped).Value;
  const auto Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 20}), 0u);
  auto After = Files->mappingSource(FD);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(After));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(After).Lease, Source.Lease);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(After).Bytes.data(),
            Source.Bytes.data());
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {FD, UINT32_MAX, UINT32_MAX}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Address, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  EXPECT_GT(Source.Lease.use_count(), 1);
  auto Unmapped = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}},
                            *Files, Result);
  ASSERT_TRUE(bool(Unmapped)) << llvm::toString(Unmapped.takeError());
  ASSERT_TRUE(*Unmapped);
  EXPECT_FALSE((**Unmapped).Error);
  EXPECT_FALSE(llvm::cantFail(Space->canAccess(Address, 1, Read)));
}

TEST_P(DarwinOwnershipTest, QueryClosuresPrecedeEveryOwnershipPreflight) {
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries}) {
    Options = darwin_test::ownershipOptions();
    Options->OwnershipMutations.reset();
    Options->Authorization = A;
    reset();
    for (auto K : Routes)
      refused(K, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX},
              A == DarwinFileAuthorization::StaticOwnerQueries
                  ? diagnostic::FileAuthorizationScope
                  : diagnostic::FileOrdinaryAuthorizationScope);
  }
}
INSTANTIATE_TEST_SUITE_P(Pages, DarwinOwnershipTest,
                         testing::Values(4096ULL, 16384ULL));
} // namespace
} // namespace neverd::emulation::darwin_model
