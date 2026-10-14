//===- DarwinOwnershipTests.cpp - Current ordinary ownership contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinDirectoryOwnershipTestData.h"
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
  void reset(uint32_t ObservedUID = 777) {
    if (Options)
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(
        *Space, Options, process_defaults::Output, ObservedUID, Credentials);
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
      O.SymbolicLinks["/symbolic"] = {100, 97, 116, 97};
      O.OwnershipMutations->Objects["/symbolic"] = {1, 2};
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
          diagnostic::FileOwnershipProvider);
  refused(ServiceKind::Fchown, {open("/"), 501, 20},
          diagnostic::FileOwnershipProvider);
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
TEST(DarwinOwnershipOptions,
     DirectoryAdmissionKeepsAliasesAndCreationIndependent) {
  auto O = darwin_test::directoryOwnershipOptions();
  EXPECT_FALSE(O.OwnershipMutations->CreatedDirectoryObjects);
  O.Metadata["/dir"].LinkCount = 7;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/alias");
  O.Metadata["/alias"] = O.Metadata["/dir"];
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Metadata["/alias"].Device = 8;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  for (unsigned Case = 0; Case != 3; ++Case) {
    auto Bad = O;
    if (Case == 0)
      Bad.Metadata.erase("/dir");
    if (Case == 1)
      Bad.Metadata["/dir"].Flags = 1;
    if (Case == 2)
      Bad.Metadata["/dir"].Mode |= 04000;
    E = validateFileOptions(Bad);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  O.OwnershipMutations->CreatedDirectoryObjects = true;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.CreationPolicy =
      darwin_test::directoryOwnershipOptions(true).CreationPolicy;
  O.InitialUmask = 022;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.CreationPolicy->Namespace.reset();
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.OwnershipMutations->CreatedDirectoryObjects = false;
  EXPECT_FALSE(bool(validateFileOptions(O)));
}
TEST_P(DarwinOwnershipTest,
       DirectoryRoutesUseLiteralRecordsAndIndependentProviders) {
  for (auto K : Routes)
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      Options = darwin_test::directoryOwnershipOptions();
      reset();
      const auto A = open("/dir"), D = ok(ServiceKind::Dup, {A}),
                 B = open("/dir");
      const auto Before = darwin_test::directoryOwnershipRecord();
      EXPECT_EQ(status(A), Before);
      EXPECT_EQ(ok(K, operands(K, High | D, High | UINT32_MAX,
                               High | UINT32_MAX, "/dir")),
                0u);
      EXPECT_EQ(status(B), Before);
      EXPECT_EQ(
          ok(K, operands(K, High | D, High | 501, High | UINT32_MAX, "/dir")),
          0u);
      EXPECT_EQ(status(A), changed(Before, 501, 31));
      EXPECT_EQ(
          ok(K, operands(K, High | D, High | UINT32_MAX, High | 20, "/dir")),
          0u);
      EXPECT_EQ(status(A), changed(Before));
      EXPECT_EQ(ok(K, operands(K, High | D, High | 501, High | 20, "/dir")),
                0u);
      EXPECT_EQ(status(B), changed(Before));
      EXPECT_EQ(ok(K, operands(K, High | D, High | UINT32_MAX,
                               High | UINT32_MAX, "/dir")),
                0u);
      EXPECT_EQ(status(D), changed(Before));
      refused(ServiceKind::Fchown, {open("/data"), 501, 20},
              diagnostic::FileOwnershipProvider);
      refused(ServiceKind::Fchmod, {D, 0750}, diagnostic::FileModeEnvironment);
      EXPECT_EQ(status(A), changed(Before));
      error(ServiceKind::Fchown, {UINT64_MAX, 501, 20}, 9);
      error(ServiceKind::FchownAt, {UINT64_MAX, 1, 501, 20, High | 8}, 22);
      error(ServiceKind::Chown, {1, 501, 20}, 14);
      EXPECT_EQ(status(D), changed(Before));
    }
}
TEST_P(DarwinOwnershipTest,
       CreatedDirectoryAndRegularOwnershipFlagsAreSeparate) {
  for (bool Regular : {false, true})
    for (bool Directory : {false, true}) {
      Options = darwin_test::directoryOwnershipOptions(true);
      Options->OwnershipMutations->CreatedRegularObjects = Regular;
      Options->OwnershipMutations->CreatedDirectoryObjects = Directory;
      reset(501);
      EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/child"), 0700}), 0u);
      const auto D = open("/child");
      const auto Before = darwin_test::directoryOwnershipRecord(true);
      EXPECT_EQ(status(D), Before);
      if (Directory) {
        EXPECT_EQ(ok(ServiceKind::Fchown, {D, 501, 20}), 0u);
        EXPECT_EQ(status(D), changed(Before, 501, 20, 101, 102));
      } else {
        refused(ServiceKind::Fchown, {D, UINT32_MAX, UINT32_MAX},
                diagnostic::FileOwnershipProvider);
        EXPECT_EQ(status(D), Before);
      }
      const auto F = open("/fresh", 0x202);
      const auto FileBefore = status(F);
      if (Regular) {
        EXPECT_EQ(ok(ServiceKind::Fchown, {F, 501, 20}), 0u);
        EXPECT_EQ(status(F), changed(FileBefore, 501, 20, 101, 102));
      } else {
        refused(ServiceKind::Fchown, {F, UINT32_MAX, UINT32_MAX},
                diagnostic::FileOwnershipProvider);
        EXPECT_EQ(status(F), FileBefore);
      }
      const auto Root = open("/");
      const auto ParentBefore = status(Root);
      refused(ServiceKind::Fchown, {Root, UINT32_MAX, UINT32_MAX},
              diagnostic::FileOwnershipProvider);
      EXPECT_EQ(status(Root), ParentBefore);
    }
}
TEST_P(DarwinOwnershipTest,
       ChangedParentGroupIsInheritedByEveryCreatedKindAndRoute) {
  for (auto K : Routes) {
    Options = darwin_test::directoryOwnershipOptions();
    Options->MutableDirectories.insert("/dir");
    Options->CreationPolicy =
        darwin_test::directoryOwnershipOptions(true).CreationPolicy;
    Options->InitialUmask = 022;
    reset(501);
    const auto D = open("/dir");
    EXPECT_EQ(status(D), darwin_test::directoryOwnershipRecord());
    EXPECT_EQ(ok(K, operands(K, D, UINT32_MAX, 20, "/dir")), 0u);
    const auto F = ok(ServiceKind::Open, {path("/dir/leaf"), 0x202, 0600});
    EXPECT_EQ(status(F), darwin_test::directoryOwnershipChildRecord(0));
    EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700}), 0u);
    const auto Sub = open("/dir/sub");
    EXPECT_EQ(status(Sub), darwin_test::directoryOwnershipChildRecord(1));
    EXPECT_EQ(
        ok(ServiceKind::Symlink, {path("leaf"), path("/dir/link", Base + 128)}),
        0u);
    const auto Link = open("/dir/link", 0x200000);
    EXPECT_EQ(status(Link), darwin_test::directoryOwnershipChildRecord(2));
    for (auto Child : {F, Sub}) {
      const auto Before = status(Child);
      refused(ServiceKind::Fchown, {Child, UINT32_MAX, UINT32_MAX},
              diagnostic::FileOwnershipProvider);
      EXPECT_EQ(status(Child), Before);
    }
    refused(ServiceKind::Fchown, {Link, UINT32_MAX, UINT32_MAX},
            diagnostic::FileOwnershipTarget);
    auto Parent = changed(darwin_test::directoryOwnershipRecord());
    llvm::support::endian::write16le(Parent.data() + 14, 5);
    llvm::support::endian::write64le(Parent.data() + 104, 80);
    llvm::support::endian::write64le(Parent.data() + 56, 81);
    llvm::support::endian::write64le(Parent.data() + 64, 82);
    EXPECT_EQ(status(D), Parent);
    // The changed group belongs to the actual directory after a direct move.
    EXPECT_EQ(
        ok(ServiceKind::Rename, {path("/dir"), path("/moved", Base + 128)}),
        0u);
    const auto Extra = open("/moved/extra", 0x202);
    EXPECT_EQ(llvm::support::endian::read32le(status(Extra).data() + 28), 20u);
    EXPECT_EQ(Options->Metadata.at("/dir").GID, 31u);
  }
}
TEST_P(DarwinOwnershipTest, DirectoryNamespaceNeverBorrowsOwnershipAuthority) {
  for (bool NamespaceTime : {false, true})
    for (bool OwnershipFirst : {false, true}) {
      Options = darwin_test::directoryOwnershipOptions();
      Options->MutableDirectories.insert("/dir");
      Options->CreationPolicy =
          darwin_test::directoryOwnershipOptions(true).CreationPolicy;
      Options->InitialUmask = 022;
      if (!NamespaceTime)
        Options->DirectoryMutationPolicies.erase("/dir");
      reset(501);
      const auto D = open("/dir");
      auto Expected = darwin_test::directoryOwnershipRecord();
      if (OwnershipFirst) {
        EXPECT_EQ(ok(ServiceKind::Fchown, {D, UINT32_MAX, 20}), 0u);
        Expected = changed(Expected);
      }
      EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700}), 0u);
      if (!NamespaceTime) {
        Record Buffer;
        Buffer.fill(0xa5);
        llvm::cantFail(Space->write(Base + Page, Buffer));
        refused(ServiceKind::Fstat64, {D, Base + Page + 8},
                diagnostic::DirectoryMutated);
        refused(ServiceKind::Fchown, {D, 501, 20},
                diagnostic::DirectoryMutated);
        Record After;
        llvm::cantFail(Space->read(Base + Page, After));
        EXPECT_EQ(After, Buffer);
        if (OwnershipFirst) {
          const auto Extra = open("/dir/extra", 0x202);
          EXPECT_EQ(llvm::support::endian::read32le(status(Extra).data() + 28),
                    20u);
        }
        continue;
      }
      llvm::support::endian::write16le(Expected.data() + 14, 3);
      llvm::support::endian::write64le(Expected.data() + 104, 48);
      for (auto Offset : {56u, 72u}) {
        llvm::support::endian::write64le(Expected.data() + Offset, 81);
        llvm::support::endian::write64le(Expected.data() + Offset + 8, 82);
      }
      EXPECT_EQ(status(D), Expected);
      EXPECT_EQ(ok(ServiceKind::Fchown, {D, 501, 20}), 0u);
      EXPECT_EQ(status(D), changed(Expected));
    }
}
TEST_P(DarwinOwnershipTest,
       DirectoryNonownerInactiveEqualAndGroupKnowledgeStayDistinct) {
  for (auto K : Routes) {
    Options = darwin_test::directoryOwnershipOptions();
    Options->Metadata["/dir"].UID = 700;
    reset();
    const auto D = open("/dir");
    auto Expected = status(D);
    error(K, operands(K, D, 501, UINT32_MAX, "/dir"), 1);
    error(K, operands(K, D, UINT32_MAX, 20, "/dir"), 1);
    EXPECT_EQ(status(D), Expected);
    EXPECT_EQ(ok(K, operands(K, D, UINT32_MAX, UINT32_MAX, "/dir")), 0u);
    EXPECT_EQ(status(D), Expected);
    EXPECT_EQ(ok(K, operands(K, D, 700, 31, "/dir")), 0u);
    Expected = changed(Expected, 700, 31);
    EXPECT_EQ(status(D), Expected);
    Options->OwnershipMutations->Objects.clear();
    reset();
    const auto Ungranted = open("/dir");
    refused(K, operands(K, Ungranted, UINT32_MAX, UINT32_MAX, "/dir"),
            diagnostic::FileOwnershipProvider);
    refused(K, operands(K, Ungranted, 700, 31, "/dir"),
            diagnostic::FileOwnershipProvider);
    Options = darwin_test::directoryOwnershipOptions();
    Credentials = darwin_test::modeCredentials();
    reset();
    const auto Known = open("/dir");
    const auto Before = status(Known);
    refused(K, operands(K, Known, UINT32_MAX, 12, "/dir"),
            diagnostic::FileOrdinaryAuthorizationGroups);
    EXPECT_EQ(status(Known), Before);
    Credentials->GroupAccessList = {20, 12};
    reset();
    const auto Positive = open("/dir");
    EXPECT_EQ(ok(K, operands(K, Positive, UINT32_MAX, 12, "/dir")), 0u);
    EXPECT_EQ(status(Positive), changed(Before, 501, 12));
    // Negative membership requires an explicit list plus authoritative UID.
    Credentials->GroupAccessList = {20};
    Credentials->GroupMembershipUID = 4294967195u;
    reset();
    const auto Negative = open("/dir");
    error(K, operands(K, Negative, UINT32_MAX, 12, "/dir"), 1);
    EXPECT_EQ(status(Negative), Before);
  }
}
TEST_P(DarwinOwnershipTest,
       DirectoryMovesAndExchangesKeepActualOwnershipGrants) {
  for (bool Swap : {false, true})
    for (bool NamespaceTime : {false, true})
      for (bool First : {false, true}) {
        Options = darwin_test::directoryOwnershipOptions();
        Options->Directories = {"/", "/a", "/b"};
        Options->Metadata.erase("/dir");
        Options->DirectoryMutationPolicies.erase("/dir");
        Options->MovableDirectories.erase("/dir");
        Options->Metadata["/a"] = darwin_test::modeMetadata(0040700, 3);
        Options->Metadata["/a"].GID = 31;
        Options->Metadata["/b"] = darwin_test::modeMetadata(0040700, 4);
        Options->MovableDirectories.insert("/a");
        Options->ExchangeableDirectories = {"/a", "/b"};
        Options->SwapRenameDirectories.insert("/");
        Options->OwnershipMutations->Objects.clear();
        Options->OwnershipMutations->Objects["/a"] = {81, 82};
        if (NamespaceTime) {
          Options->DirectoryMutationPolicies["/a"] = {16, {91, 92}};
          Options->DirectoryMutationPolicies["/b"] = {16, {101, 102}};
        }
        reset();
        const auto A = open("/a"), D = ok(ServiceKind::Dup, {A}),
                   B = open("/b");
        auto Expected = status(A), Other = status(B);
        if (First) {
          EXPECT_EQ(ok(ServiceKind::Fchown, {A, UINT32_MAX, 20}), 0u);
          Expected = changed(Expected);
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
          refused(ServiceKind::Fchown, {D, 501, 20},
                  diagnostic::DirectoryMutated);
          continue;
        }
        Expected = changed(Expected, 501, First ? 20 : 31, 91, 92);
        EXPECT_EQ(status(D), Expected);
        EXPECT_EQ(ok(ServiceKind::Fchown, {D, 501, 20}), 0u);
        Expected = changed(Expected);
        EXPECT_EQ(status(open(Swap ? "/b" : "/moved")), Expected);
        EXPECT_EQ(status(A), Expected);
        if (Swap)
          Other = changed(Other, 501, 20, 101, 102);
        EXPECT_EQ(status(B), Other);
        refused(ServiceKind::Fchown, {B, UINT32_MAX, UINT32_MAX},
                diagnostic::FileOwnershipProvider);
        EXPECT_EQ(status(B), Other);
      }
}
TEST_P(DarwinOwnershipTest,
       HeldRemovedDirectoryAndReplacementNeverShareOwnershipGrants) {
  Options = darwin_test::directoryOwnershipOptions();
  Options->RemovableDirectories.insert("/dir");
  Options->CreationPolicy =
      darwin_test::directoryOwnershipOptions(true).CreationPolicy;
  Options->InitialUmask = 022;
  reset(501);
  const auto A = open("/dir"), D = ok(ServiceKind::Dup, {A});
  auto Expected = darwin_test::directoryOwnershipRecord();
  EXPECT_EQ(ok(ServiceKind::Rmdir, {path("/dir")}), 0u);
  Expected = changed(Expected, 501, 31);
  EXPECT_EQ(status(D), Expected);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  const auto New = open("/dir");
  const auto Before = status(New);
  refused(ServiceKind::Fchown, {New, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipProvider);
  EXPECT_EQ(status(New), Before);
  EXPECT_EQ(ok(ServiceKind::Fchown, {D, UINT32_MAX, 20}), 0u);
  EXPECT_EQ(status(A), changed(Expected));
  EXPECT_EQ(status(New), Before);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Fchown, {A, 501, 20}, 9);
  EXPECT_EQ(ok(ServiceKind::Fchown, {D, UINT32_MAX, UINT32_MAX}), 0u);
}
TEST_P(DarwinOwnershipTest, DirectoryEnumerationAndChildrenKeepTheirOwnState) {
  Options = darwin_test::directoryOwnershipOptions();
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
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {A, Base + Page * 2, 32, Base + 512}),
      32u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {D, UINT32_MAX, 20}), 0u);
  EXPECT_EQ(status(B), changed(Before));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Flags);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 11u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Chown, {path("."), 501, 20}), 0u);
  for (auto F : {open("leaf"), open("sub")}) {
    const auto Child = status(F);
    refused(ServiceKind::Fchown, {F, UINT32_MAX, UINT32_MAX},
            diagnostic::FileOwnershipProvider);
    EXPECT_EQ(status(F), Child);
  }
}

TEST_P(DarwinOwnershipTest, CreatedDirectoryGroupChangesGovernEveryChildKind) {
  for (auto K : Routes) {
    Options = darwin_test::directoryOwnershipOptions(true);
    Credentials = darwin_test::modeCredentials();
    Credentials->GroupAccessList = {20, 31};
    reset(501);
    error(ServiceKind::Mkdir, {path("/"), 0700}, 17);
    error(ServiceKind::Mkdir, {1, 0700}, 14);
    EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
    const auto D = open("/dir");
    const auto Before = darwin_test::directoryOwnershipRecord(true);
    EXPECT_EQ(status(D), Before);
    EXPECT_EQ(ok(K, operands(K, D, UINT32_MAX, 31, "/dir")), 0u);
    EXPECT_EQ(status(D), changed(Before, 501, 31, 101, 102));
    const auto F = ok(ServiceKind::Open, {path("/dir/leaf"), 0x202, 0600});
    auto File = darwin_test::directoryOwnershipChildRecord(0);
    llvm::support::endian::write64le(File.data() + 16, 101);
    llvm::support::endian::write32le(File.data() + 28, 31);
    EXPECT_EQ(status(F), File);
    EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700}), 0u);
    const auto Sub = open("/dir/sub");
    auto Directory = darwin_test::directoryOwnershipChildRecord(1);
    llvm::support::endian::write64le(Directory.data() + 16, 102);
    llvm::support::endian::write32le(Directory.data() + 28, 31);
    EXPECT_EQ(status(Sub), Directory);
    EXPECT_EQ(
        ok(ServiceKind::Symlink, {path("leaf"), path("/dir/link", Base + 128)}),
        0u);
    const auto L = open("/dir/link", 0x200000);
    auto Link = darwin_test::directoryOwnershipChildRecord(2);
    llvm::support::endian::write64le(Link.data() + 16, 103);
    llvm::support::endian::write32le(Link.data() + 28, 31);
    EXPECT_EQ(status(L), Link);
    // Only the independent global created-directory flag grants this child.
    EXPECT_EQ(ok(ServiceKind::Fchown, {Sub, UINT32_MAX, UINT32_MAX}), 0u);
    EXPECT_EQ(status(Sub), Directory);
    refused(ServiceKind::Fchown, {F, UINT32_MAX, UINT32_MAX},
            diagnostic::FileOwnershipProvider);
    EXPECT_EQ(status(F), File);
    refused(ServiceKind::Fchown, {L, UINT32_MAX, UINT32_MAX},
            diagnostic::FileOwnershipTarget);
    EXPECT_EQ(status(L), Link);
  }
}

TEST_P(DarwinOwnershipTest,
       DirectoryAttributeAndNamespaceTimesRemainIndependent) {
  Options = darwin_test::directoryOwnershipOptions();
  Options->MutableDirectories.insert("/dir");
  Options->DirectoryMutationPolicies["/dir"].Time = {91, 92};
  Options->ModeMutations.emplace().Objects["/dir"] = {71, 72};
  Options->TimestampMutations.emplace().Objects["/dir"] = {111, 112};
  reset();
  const auto D = open("/dir");
  const auto Before = darwin_test::directoryOwnershipRecord();
  EXPECT_EQ(status(D), Before);
  EXPECT_EQ(ok(ServiceKind::Fchown, {D, 501, 31}), 0u);
  EXPECT_EQ(status(D), changed(Before, 501, 31));
  EXPECT_EQ(ok(ServiceKind::Fchmod, {D, 0750}), 0u);
  auto Expected = changed(Before, 501, 31, 71, 72);
  llvm::support::endian::write16le(Expected.data() + 12, 0040750);
  EXPECT_EQ(status(D), Expected);
  std::array<uint8_t, 32> Input;
  Input.fill(0xa5);
  llvm::support::endian::write64le(Input.data(), 11);
  llvm::support::endian::write32le(Input.data() + 8, 12);
  llvm::support::endian::write64le(Input.data() + 16, 13);
  llvm::support::endian::write32le(Input.data() + 24, 14);
  llvm::cantFail(Space->write(Base + 256, Input));
  EXPECT_EQ(ok(ServiceKind::Futimes, {D, Base + 256}), 0u);
  Expected = changed(Expected, 501, 31, 111, 112);
  llvm::support::endian::write64le(Expected.data() + 40, 11);
  llvm::support::endian::write64le(Expected.data() + 48, 12000);
  llvm::support::endian::write64le(Expected.data() + 56, 13);
  llvm::support::endian::write64le(Expected.data() + 64, 14000);
  EXPECT_EQ(status(D), Expected);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700}), 0u);
  Expected = changed(Expected, 501, 31, 91, 92);
  llvm::support::endian::write16le(Expected.data() + 14, 3);
  llvm::support::endian::write64le(Expected.data() + 104, 48);
  llvm::support::endian::write64le(Expected.data() + 56, 91);
  llvm::support::endian::write64le(Expected.data() + 64, 92);
  EXPECT_EQ(status(D), Expected);
  EXPECT_EQ(ok(ServiceKind::Fchown, {D, UINT32_MAX, 20}), 0u);
  EXPECT_EQ(status(D), changed(Expected));
  std::array<uint8_t, 32> After;
  llvm::cantFail(Space->read(Base + 256, After));
  EXPECT_EQ(After, Input);
}

INSTANTIATE_TEST_SUITE_P(Pages, DarwinOwnershipTest,
                         testing::Values(4096ULL, 16384ULL));
} // namespace
} // namespace neverd::emulation::darwin_model
