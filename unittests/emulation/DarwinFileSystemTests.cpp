//===- DarwinFileSystemTests.cpp - Declared filesystem snapshots -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinSymbolicLinkFileSystemTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinFileSystemTest
    : public testing::TestWithParam<std::tuple<uint64_t, bool>> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  bool FDRoute;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "filesystem"};
  void SetUp() override {
    std::tie(Page, FDRoute) = GetParam();
    auto P = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    auto S = AddressSpace::create(*P, Page * 16);
    ASSERT_TRUE(bool(S)) << llvm::toString(S.takeError());
    Space = std::move(*S);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::fileSystemOptions();
    Credentials = darwin_test::modeCredentials();
    reset();
  }
  void reset() {
    if (Options)
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(
        *Space, Options, process_defaults::Output, 501, Credentials);
  }
  uint64_t path(llvm::StringRef T, uint64_t A = Base) {
    const auto S = T.str() + '\0';
    llvm::cantFail(Space->write(
        A, {reinterpret_cast<const uint8_t *>(S.data()), S.size()}));
    return A;
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result =
        ProcessResult{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                      ExecutionBackendKind::Unicorn, "filesystem"};
    auto O = Files->handle(K, {0, 0, A, {}}, Result);
    EXPECT_TRUE(bool(O)) << (O ? "" : llvm::toString(O.takeError()));
    return O ? *O : std::nullopt;
  }
  uint64_t ok(ServiceKind K, std::array<uint64_t, 6> A) {
    auto O = invoke(K, A);
    EXPECT_TRUE(O) << Result.Diagnostic;
    EXPECT_FALSE(O && O->Error);
    return O ? O->Value : UINT64_MAX;
  }
  void error(ServiceKind K, std::array<uint64_t, 6> A, uint32_t N) {
    auto O = invoke(K, A);
    ASSERT_TRUE(O) << Result.Diagnostic;
    EXPECT_TRUE(O->Error);
    EXPECT_EQ(O->Value, N);
  }
  void refused(ServiceKind K, std::array<uint64_t, 6> A, const char *Reason) {
    EXPECT_FALSE(invoke(K, A));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(Result.Diagnostic, Reason);
  }
  uint64_t open(llvm::StringRef N = "/data", uint32_t F = 0) {
    return ok(ServiceKind::Open, {path(N), F, 0600});
  }
  ServiceKind route() const {
    return FDRoute ? ServiceKind::Fstatfs64 : ServiceKind::Statfs64;
  }
  std::array<uint64_t, 6> args(uint64_t FD, uint64_t Out,
                               llvm::StringRef N = "/data") {
    return {FDRoute ? FD : path(N),
            Out,
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX,
            UINT64_MAX};
  }
  using Record = std::array<uint8_t, 2200>;
  Record capture(uint64_t FD, llvm::StringRef N = "/data", uint64_t High = 0) {
    Record B;
    B.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, B));
    EXPECT_EQ(ok(route(), args(High | FD, Base + Page + 16, N)), 0u);
    llvm::cantFail(Space->read(Base + Page, B));
    return B;
  }
  Record captureLink(uint64_t FD) {
    Record B;
    B.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, B));
    EXPECT_EQ(ok(ServiceKind::Fstatfs64, {FD, Base + Page + 16, UINT64_MAX,
                                          UINT64_MAX, UINT64_MAX, UINT64_MAX}),
              0u);
    llvm::cantFail(Space->read(Base + Page, B));
    return B;
  }
  std::array<uint8_t, 160> status(uint64_t FD) {
    std::array<uint8_t, 160> B;
    B.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page * 2, B));
    EXPECT_EQ(ok(ServiceKind::Fstat64, {FD, Base + Page * 2 + 8}), 0u);
    llvm::cantFail(Space->read(Base + Page * 2, B));
    return B;
  }
  void policies(bool Created = false) {
    Options->WritableFiles.insert("/data");
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
    Options->FileSystemObservations->CreatedRegularObjects = Created;
    reset();
  }
};
TEST(DarwinFileSystemOptions, AdmissionAndClosedQueryDomains) {
  const auto Initial = darwin_test::fileSystemOptions();
  EXPECT_FALSE(Initial.FileSystemObservations->CreatedRegularObjects);
  EXPECT_FALSE(bool(validateFileOptions(Initial)));
  for (unsigned I = 0; I != 12; ++I) {
    auto O = Initial;
    auto &F = *O.FileSystemObservations;
    switch (I) {
    case 0:
      F.Objects["/missing"] = {};
      break;
    case 1:
      O.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
      F.Objects["/link/child"] = {};
      break;
    case 2:
      F.Objects["/data"].TypeName = std::string(16, 'x');
      break;
    case 3:
      F.Objects["/data"].MountPoint = std::string(1024, 'x');
      break;
    case 4:
      F.Objects["/data"].MountedFrom = std::string(1024, 'x');
      break;
    case 5:
      F.Objects["/data"].TypeName = std::string("a\0b", 3);
      break;
    case 6:
      F.Objects["/data"].MountPoint = std::string(1, char(0xff));
      break;
    case 7:
      F.CreatedRegularObjects = true;
      break;
    case 8:
      O.Authorization = DarwinFileAuthorization::StaticOwnerQueries;
      break;
    case 9:
      O.Authorization = DarwinFileAuthorization::StaticOrdinaryQueries;
      break;
    case 10:
      F.Objects.clear();
      O.Authorization = DarwinFileAuthorization::StaticOwnerQueries;
      break;
    case 11:
      F.Objects.clear();
      O.Authorization = DarwinFileAuthorization::StaticOrdinaryQueries;
      break;
    }
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << I;
    llvm::consumeError(std::move(E));
  }
}
TEST(DarwinFileSystemOptions, IndependentMetadataAndSharedSnapshotBudget) {
  auto O = darwin_test::fileSystemOptions();
  O.Metadata.clear();
  O.Files["/implicit/leaf"] = {};
  O.FileSystemObservations->Objects["/implicit"] = {};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  auto Invalid = O;
  Invalid.FileSystemObservations->Objects["/implicit//leaf"] = {};
  auto E = validateFileOptions(Invalid);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.FileSystemObservations->Objects["/data"] = {};
  // Find the exact admission boundary without borrowing private footprint.
  uint64_t Low = 0, High = darwin_file_limits::Bytes;
  while (Low < High) {
    const auto Mid = (Low + High + 1) / 2;
    O.Files["/data"].resize(Mid);
    auto Err = validateFileOptions(O);
    const bool Good = !Err;
    llvm::consumeError(std::move(Err));
    if (Good)
      Low = Mid;
    else
      High = Mid - 1;
  }
  O.Files["/data"].resize(Low);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files["/data"].resize(Low);
  O.FileSystemObservations->Objects.erase("/implicit");
  O.Files["/data"].resize(Low + 2168 + 10);
  EXPECT_FALSE(bool(validateFileOptions(O)));
}
TEST(DarwinFileSystemOptions, ObservedImplicitDirectoriesShareEntryBudget) {
  DarwinFileOptions O;
  for (unsigned I = 0; I != 254; ++I)
    O.Files["/dir/f" + std::to_string(I)] = {};
  O.FileSystemObservations.emplace().Objects["/dir"] = {};
  O.FileSystemObservations->Objects["/"] = {};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  auto Excess = O;
  Excess.Files["/dir/extra"] = {};
  auto E = validateFileOptions(Excess);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)),
            std::string(diagnostic::FileOptionsLimit));
  // A directory already counted by another declaration is never counted twice.
  for (unsigned I = 0; I != 5; ++I) {
    auto Shared = O;
    switch (I) {
    case 0:
      Shared.Directories.insert("/dir");
      break;
    case 1:
      Shared.Metadata["/dir"] = darwin_test::modeMetadata(0040700, 1);
      break;
    case 2:
      Shared.DirectoryContents["/dir"].MinimumBufferSize = 24;
      Shared.DirectoryContents["/dir"].Entries = {{".", 1, 4, 1},
                                                  {"..", 2, 4, 2}};
      for (unsigned N = 0; N != 254; ++N)
        Shared.DirectoryContents["/dir"].Entries.push_back(
            {"f" + std::to_string(N), N + 3, 8, N + 3});
      break;
    case 3:
      Shared.ExtendedAttributes["/dir"] = {};
      break;
    case 4:
      Shared.MutableDirectories.insert("/dir");
      break;
    }
    EXPECT_FALSE(bool(validateFileOptions(Shared))) << I;
  }
}

TEST_P(DarwinFileSystemTest, FullGuardedRecordAndLowCarriers) {
  const auto FD = open();
  for (uint64_t H : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
    EXPECT_EQ(capture(FD, "/data", H), darwin_test::fileSystemRecord());
}
TEST_P(DarwinFileSystemTest, ZeroValuesEmptyStringsAndMaximumNamesAreExplicit) {
  Options->FileSystemObservations->Objects["/data"] = {};
  reset();
  auto FD = open();
  auto B = capture(FD);
  for (unsigned I = 16; I != 2184; ++I)
    EXPECT_EQ(B[I], 0);
  for (unsigned I = 0; I != 16; ++I) {
    EXPECT_EQ(B[I], 0xa5);
    EXPECT_EQ(B[I + 2184], 0xa5);
  }
  auto &S = Options->FileSystemObservations->Objects["/data"];
  S.TypeName = std::string(15, 't');
  S.MountPoint = std::string(1023, 'm');
  S.MountedFrom = std::string(1023, 'f');
  reset();
  FD = open();
  B = capture(FD);
  EXPECT_EQ(B[16 + 87], 0);
  EXPECT_EQ(B[16 + 1111], 0);
  EXPECT_EQ(B[16 + 2135], 0);
  EXPECT_EQ(B[16 + 72], 't');
  EXPECT_EQ(B[16 + 88], 'm');
  EXPECT_EQ(B[16 + 1112], 'f');
}
TEST_P(DarwinFileSystemTest, KnownObjectBeforeBadOutputAndPartialHasNoEffects) {
  const auto FD = open();
  error(route(), args(FD, 1), 14);
  error(route(), args(FD, 0), 14);
  if (FDRoute) {
    error(route(), args(UINT64_MAX, 1), 9);
    ok(ServiceKind::Close, {FD});
    error(route(), args(FD, 1), 9);
  } else {
    error(route(), args(FD, 1, "/absent"), 2);
    error(route(), args(FD, 1, ""), 2);
  }
  std::array<uint8_t, 32> B;
  B.fill(0x6c);
  llvm::cantFail(Space->write(Base + Page * 4 - 32, B));
  const auto Valid = FDRoute ? open() : FD;
  refused(route(), args(Valid, Base + Page * 4 - 32),
          diagnostic::FileSystemPartialOutput);
  std::array<uint8_t, 32> A;
  llvm::cantFail(Space->read(Base + Page * 4 - 32, A));
  EXPECT_EQ(A, B);
}
TEST_P(DarwinFileSystemTest, MissingEmptyAndDifferentPurposeNeverGrant) {
  for (bool Missing : {false, true}) {
    if (Missing)
      Options->FileSystemObservations.reset();
    else
      Options->FileSystemObservations.emplace();
    Options->Synchronization.emplace().Objects.insert("/data");
    reset();
    const auto FD = open();
    refused(route(), args(FD, 1),
            Missing ? diagnostic::FileSystemEnvironment
                    : diagnostic::FileSystemProvider);
  }
}
TEST_P(DarwinFileSystemTest, NoTargetStatOrAccessFlagRequirement) {
  Options->Metadata.erase("/data");
  Options->WritableFiles.insert("/data");
  Options->FileSystemObservations->Objects["/"] =
      darwin_test::fileSystemSnapshot();
  if (FDRoute)
    Credentials.reset();
  reset();
  for (uint32_t F : {0u, 1u, 2u}) {
    const auto FD = open("/data", F);
    EXPECT_EQ(capture(FD), darwin_test::fileSystemRecord());
    refused(ServiceKind::Fstat64, {FD, Base + Page * 2},
            diagnostic::FileMetadata);
    ok(ServiceKind::Close, {FD});
  }
}
TEST_P(DarwinFileSystemTest, EffectiveSearchUsesAuthoritativeGroupDecision) {
  const auto Query = [&] {
    return ok(ServiceKind::Statfs64, {path("/data"), Base + Page + 16});
  };
  auto &M = Options->Metadata["/"];
  M.UID = 700;
  M.GID = 31;
  M.Mode = 0040010;
  Credentials->GroupAccessList = std::vector<uint32_t>{20, 31};
  reset();
  EXPECT_EQ(Query(), 0u);
  Credentials->GroupAccessList = std::vector<uint32_t>{20};
  Credentials->GroupMembershipUID = 4294967195u;
  reset();
  error(ServiceKind::Statfs64, {path("/data"), 1}, 13);
  Credentials->GroupMembershipUID.reset();
  reset();
  refused(ServiceKind::Statfs64, {path("/data"), 1},
          diagnostic::FileOrdinaryAuthorizationGroups);
  M.Mode = 0040001;
  Credentials->GroupMembershipUID = 4294967195u;
  reset();
  EXPECT_EQ(Query(), 0u);
}
TEST_P(DarwinFileSystemTest, MissingRootSpecialAndUnknownAncestorFailClearly) {
  Credentials.reset();
  reset();
  refused(ServiceKind::Statfs64, {path("/data"), 1},
          diagnostic::FileOrdinaryAuthorizationCredentials);
  Credentials = darwin_test::modeCredentials();
  Credentials->EffectiveUID = 0;
  reset();
  refused(ServiceKind::Statfs64, {path("/data"), 1},
          diagnostic::FileOrdinaryAuthorizationSubject);
  Credentials = darwin_test::modeCredentials();
  Options->Metadata["/"].Mode |= 01000;
  reset();
  refused(ServiceKind::Statfs64, {path("/data"), 1},
          diagnostic::FileSystemSecurityAttributes);
  Options->Metadata.erase("/");
  reset();
  refused(ServiceKind::Statfs64, {path("/data"), 1}, diagnostic::FileMetadata);
}
TEST_P(DarwinFileSystemTest, FollowedLinksCWDAndImplicitDirectoryBinding) {
  Options->Files["/dir/leaf"] = {};
  Options->Metadata["/dir"] = darwin_test::modeMetadata(0040700, 10);
  Options->FileSystemObservations->Objects["/dir"] =
      darwin_test::fileSystemSnapshot();
  Options->FileSystemObservations->Objects["/dir/leaf"] =
      darwin_test::fileSystemSnapshot();
  Options->SymbolicLinks["/alias"] = {'d', 'i', 'r', '/', 'l', 'e', 'a', 'f'};
  reset();
  auto FD = open("/alias");
  EXPECT_EQ(capture(FD, "/alias"), darwin_test::fileSystemRecord());
  ok(ServiceKind::Chdir, {path("/dir")});
  FD = open("leaf");
  EXPECT_EQ(capture(FD, "leaf"), darwin_test::fileSystemRecord());
  const auto Dir = open(".");
  EXPECT_EQ(capture(Dir, "."), darwin_test::fileSystemRecord());
}
TEST_P(DarwinFileSystemTest,
       DuplicatesSeparateDescriptionsKeepCursorFlagsAndMetadata) {
  const auto A = open(), B = open();
  const auto D = ok(ServiceKind::Dup, {A});
  ok(ServiceKind::Lseek, {A, 2, 0});
  ok(ServiceKind::Fcntl, {D, 2, 1});
  const auto Before = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {D, 3});
  EXPECT_EQ(capture(D), darwin_test::fileSystemRecord());
  EXPECT_EQ(status(B), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Flags);
  const auto F = ok(ServiceKind::Fcntl, {D, 0, 10});
  EXPECT_EQ(capture(F), darwin_test::fileSystemRecord());
}
TEST_P(DarwinFileSystemTest, InitialChildrenAndEqualIDsConferNothing) {
  Options->Files["/other"] = Options->Files["/data"];
  Options->Metadata["/other"] = Options->Metadata["/data"];
  reset();
  const auto FD = open("/other");
  refused(route(), args(FD, 1, "/other"), diagnostic::FileSystemProvider);
}
TEST_P(DarwinFileSystemTest, HardlinkRenameHeldUnlinkAndNameReuse) {
  policies();
  const auto A = open(), D = ok(ServiceKind::Dup, {A});
  ok(ServiceKind::Link, {path("/data"), path("/linked", Base + 128)});
  EXPECT_EQ(capture(D, "/linked"), darwin_test::fileSystemRecord());
  ok(ServiceKind::Rename, {path("/linked"), path("/moved", Base + 128)});
  EXPECT_EQ(capture(A, "/moved"), darwin_test::fileSystemRecord());
  ok(ServiceKind::Unlink, {path("/data")});
  ok(ServiceKind::Unlink, {path("/moved")});
  if (FDRoute)
    EXPECT_EQ(capture(D), darwin_test::fileSystemRecord());
  else
    error(ServiceKind::Statfs64, {path("/data"), 1}, 2);
  const auto New = open("/data", 0x202);
  refused(route(), args(New, 1), diagnostic::FileSystemProvider);
  if (FDRoute)
    EXPECT_EQ(capture(A), darwin_test::fileSystemRecord());
  ok(ServiceKind::Close, {A});
  ok(ServiceKind::Close, {D});
  error(ServiceKind::Fstatfs64, {A, 1}, 9);
}
TEST_P(DarwinFileSystemTest,
       CreationExplicitlyUsesBoundParentAndDefaultsClosed) {
  policies();
  const auto Old = open();
  const auto N = open("/new", 0x202);
  refused(route(), args(N, 1, "/new"), diagnostic::FileSystemProvider);
  policies(true);
  const auto New = open("/new", 0x202);
  EXPECT_EQ(ok(ServiceKind::Fstatfs64, {New, Base + Page + 16}), 0u);
  Record B;
  B.fill(0xa5);
  llvm::cantFail(Space->write(Base + Page, B));
  EXPECT_EQ(ok(ServiceKind::Fstatfs64, {New, Base + Page + 16}), 0u);
  llvm::cantFail(Space->read(Base + Page, B));
  EXPECT_EQ(B, darwin_test::fileSystemRecord());
  EXPECT_EQ(capture(New, "/new"), darwin_test::fileSystemRecord());
  Options->FileSystemObservations->Objects.erase("/");
  policies(true);
  const auto Unbound = open("/new", 0x202);
  refused(ServiceKind::Fstatfs64, {Unbound, 1}, diagnostic::FileSystemProvider);
  (void)Old;
}
TEST_P(DarwinFileSystemTest,
       DirectoryMoveRetainsBindingAndNewDirectoriesReceiveNone) {
  Options->Directories.insert("/dir");
  Options->Metadata["/dir"] = darwin_test::modeMetadata(0040700, 10);
  Options->FileSystemObservations->Objects["/dir"] =
      darwin_test::fileSystemSnapshot();
  Options->MutableDirectories = {"/", "/dir"};
  Options->MovableDirectories.insert("/dir");
  Options->DirectoryMutationPolicies["/dir"] = {16, {91, 92}};
  policies(true);
  const auto Dir = open("/dir");
  ok(ServiceKind::Rename, {path("/dir"), path("/moved", Base + 128)});
  Record B;
  B.fill(0xa5);
  llvm::cantFail(Space->write(Base + Page, B));
  EXPECT_EQ(ok(ServiceKind::Fstatfs64, {Dir, Base + Page + 16}), 0u);
  llvm::cantFail(Space->read(Base + Page, B));
  EXPECT_EQ(B, darwin_test::fileSystemRecord());
  ok(ServiceKind::Mkdir, {path("/newdir"), 0700});
  const auto NewDir = open("/newdir");
  refused(ServiceKind::Fstatfs64, {NewDir, 1}, diagnostic::FileSystemProvider);
  const auto Child = open("/newdir/leaf", 0x202);
  refused(ServiceKind::Fstatfs64, {Child, 1}, diagnostic::FileSystemProvider);
}
TEST_P(DarwinFileSystemTest,
       DirtyBytesLocksXattrsAndMetadataValidityRemainIndependent) {
  Options->ExtendedAttributes["/data"] = {{"user.test", {4, 5}}};
  Options->AdvisoryLocks.emplace().Objects.insert("/data");
  policies();
  const auto A = open("/data", 2), B = open("/data", 2);
  const auto Before = status(A);
  ok(ServiceKind::Flock, {A, 2});
  error(ServiceKind::Flock, {B, 2 | 4}, 35);
  EXPECT_EQ(capture(A), darwin_test::fileSystemRecord());
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(
      ok(ServiceKind::FgetXattr, {A, path("user.test", Base + 128), 0, 0}), 2u);
  path("Z", Base + 512);
  ok(ServiceKind::Pwrite, {A, Base + 512, 1, 0});
  refused(ServiceKind::Fstat64, {A, Base + Page * 2},
          diagnostic::FileMutatedMetadata);
  EXPECT_EQ(capture(B), darwin_test::fileSystemRecord());
  error(ServiceKind::Flock, {B, 2 | 4}, 35);
  ok(ServiceKind::Flock, {A, 8});
  refused(ServiceKind::FgetXattr, {B, path("user.test", Base + 128), 0, 0},
          diagnostic::ExtendedAttributeUnknown);
  std::array<uint8_t, 6> Data;
  ok(ServiceKind::Pread, {B, Base + 512, 6, 0});
  llvm::cantFail(Space->read(Base + 512, Data));
  EXPECT_EQ(Data, (std::array<uint8_t, 6>{'Z', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinFileSystemTest,
       RealMappingRetainsBytesWithoutDescriptorAuthority) {
  policies();
  const auto FD = open();
  ProcessOptions O;
  O.MemoryLimit = Page * 16;
  O.StackSize = Page;
  MemoryLayout Layout{Page, Base + Page * 8, {}};
  DarwinMemory VM(*Space, Layout, O);
  auto M = VM.handle(ServiceKind::Mmap, {0, 0, {0, Page, 1, 2, FD, 0}, {}},
                     *Files, Result);
  ASSERT_TRUE(bool(M)) << llvm::toString(M.takeError());
  ASSERT_TRUE(*M);
  ASSERT_FALSE((**M).Error);
  const auto Address = (**M).Value,
             Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(capture(FD), darwin_test::fileSystemRecord());
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  ok(ServiceKind::Unlink, {path("/data")});
  EXPECT_EQ(ok(ServiceKind::Fstatfs64, {FD, Base + Page + 16}), 0u);
  ok(ServiceKind::Close, {FD});
  error(ServiceKind::Fstatfs64, {FD, 1}, 9);
  std::array<uint8_t, 6> Data;
  llvm::cantFail(Space->read(Address, Data));
  EXPECT_EQ(Data, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  auto U = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}}, *Files,
                     Result);
  ASSERT_TRUE(bool(U)) << llvm::toString(U.takeError());
  ASSERT_TRUE(*U);
  EXPECT_FALSE((**U).Error);
}
TEST_P(DarwinFileSystemTest,
       OpaqueStreamsRemainUnsupportedAndUnknownSymbolicSnapshotsStayClosed) {
  for (uint64_t FD : {0u, 1u, 2u})
    refused(ServiceKind::Fstatfs64, {FD, 1}, diagnostic::FileSystemKind);
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  reset();
  const auto FD = open("/link", 0x200000);
  refused(ServiceKind::Fstatfs64, {FD, 1}, diagnostic::FileSystemProvider);
}

TEST(DarwinFileSystemOptions, CreatedDirectoryFlagNeedsCompleteNamespace) {
  auto O = darwin_test::fileSystemOptions();
  EXPECT_FALSE(O.FileSystemObservations->CreatedDirectoryObjects);
  O.FileSystemObservations->CreatedDirectoryObjects = true;
  auto E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
  O.CreationPolicy =
      darwin_test::createdDirectoryFileSystemOptions().CreationPolicy;
  O.MutableDirectories.insert("/");
  O.InitialUmask = 022;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.CreationPolicy->Namespace.reset();
  E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
  O.FileSystemObservations->CreatedDirectoryObjects = false;
  EXPECT_FALSE(bool(validateFileOptions(O)));
}

TEST_P(DarwinFileSystemTest,
       CreatedDirectoryAndRegularSnapshotFlagsAreSeparate) {
  for (bool Directory : {false, true})
    for (bool Regular : {false, true}) {
      Options =
          darwin_test::createdDirectoryFileSystemOptions(Directory, Regular);
      reset();
      const auto Initial = open();
      refused(route(), args(Initial, 1), diagnostic::FileSystemProvider);
      ok(ServiceKind::Mkdir, {path("/dir"), 0700});
      const auto Dir = open("/dir");
      if (Directory)
        EXPECT_EQ(capture(Dir, "/dir"), darwin_test::fileSystemRecord());
      else
        refused(route(), args(Dir, 1, "/dir"), diagnostic::FileSystemProvider);
      const auto Child = open("/dir/leaf", 0x202);
      if (Directory && Regular)
        EXPECT_EQ(capture(Child, "/dir/leaf"), darwin_test::fileSystemRecord());
      else
        refused(route(), args(Child, 1, "/dir/leaf"),
                diagnostic::FileSystemProvider);
      ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700});
      const auto Grandchild = open("/dir/sub");
      if (Directory)
        EXPECT_EQ(capture(Grandchild, "/dir/sub"),
                  darwin_test::fileSystemRecord());
      else
        refused(route(), args(Grandchild, 1, "/dir/sub"),
                diagnostic::FileSystemProvider);
      ok(ServiceKind::Symlink, {path("leaf"), path("/dir/link", Base + 128)});
      const auto Link = open("/dir/link", 0x200000);
      refused(ServiceKind::Fstatfs64, {Link, 1},
              diagnostic::FileSystemProvider);
    }
}

TEST_P(DarwinFileSystemTest,
       CreatedDirectoriesDistinguishZeroAndUnboundParents) {
  for (bool Bound : {false, true}) {
    Options = darwin_test::createdDirectoryFileSystemOptions(true, true);
    Options->FileSystemObservations->Objects.clear();
    if (Bound)
      Options->FileSystemObservations->Objects["/"] = {};
    reset();
    ok(ServiceKind::Mkdir, {path("/dir"), 0700});
    const auto Dir = open("/dir");
    const auto Leaf = open("/dir/leaf", 0x202);
    for (const auto &V :
         {std::pair<uint64_t, const char *>{Dir, "/dir"}, {Leaf, "/dir/leaf"}})
      if (Bound) {
        Record Expected{};
        std::fill_n(Expected.begin(), 16, 0xa5);
        std::fill_n(Expected.end() - 16, 16, 0xa5);
        EXPECT_EQ(capture(V.first, V.second), Expected);
        error(route(), args(V.first, 1, V.second), 14);
      } else
        refused(route(), args(V.first, 1, V.second),
                diagnostic::FileSystemProvider);
  }
}

TEST_P(DarwinFileSystemTest,
       CreatedDirectoriesUseMovedAndExchangedActualParent) {
  for (bool Swap : {false, true}) {
    Options = darwin_test::createdDirectoryFileSystemOptions(true, true);
    for (const auto &V :
         {std::pair<const char *, uint64_t>{"/a", 10}, {"/b", 11}}) {
      Options->Directories.insert(V.first);
      Options->Metadata[V.first] = darwin_test::modeMetadata(0040700, V.second);
      Options->MutableDirectories.insert(V.first);
      Options->DirectoryMutationPolicies[V.first] = {16, {91, 92}};
    }
    Options->MovableDirectories.insert("/a");
    Options->ExchangeableDirectories = {"/a", "/b"};
    Options->Directories.insert("/");
    Options->SwapRenameDirectories.insert("/");
    Options->FileSystemObservations->Objects["/a"] =
        darwin_test::fileSystemSnapshot();
    Options->FileSystemObservations->Objects["/a"].Owner = 17;
    Options->FileSystemObservations->Objects["/b"] =
        darwin_test::fileSystemSnapshot();
    Options->FileSystemObservations->Objects["/b"].Owner = 23;
    reset();
    const auto Parent = open("/a"), Other = open("/b");
    const auto From = path("/a"), To = path(Swap ? "/b" : "/moved", Base + 128);
    if (Swap)
      ok(ServiceKind::RenameAtX, {UINT64_MAX, From, UINT64_MAX, To, 2});
    else
      ok(ServiceKind::Rename, {From, To});
    const std::string Name = std::string(Swap ? "/b" : "/moved") + "/child";
    ok(ServiceKind::Mkdir, {path(Name), 0700});
    const auto Child = open(Name);
    EXPECT_EQ(capture(Child, Name), darwin_test::directoryFileSystemRecord(17));
    const auto LeafName = Name + "/leaf";
    const auto Leaf = open(LeafName, 0x202);
    EXPECT_EQ(capture(Leaf, LeafName),
              darwin_test::directoryFileSystemRecord(17));
    if (FDRoute) {
      EXPECT_EQ(capture(Parent), darwin_test::directoryFileSystemRecord(17));
      EXPECT_EQ(capture(Other), darwin_test::directoryFileSystemRecord(23));
    }
    // A later child move to root preserves the original bound snapshot.
    ok(ServiceKind::Rename, {path(Name), path("/child", Base + 128)});
    EXPECT_EQ(capture(Child, "/child"),
              darwin_test::directoryFileSystemRecord(17));
    EXPECT_EQ(capture(Leaf, "/child/leaf"),
              darwin_test::directoryFileSystemRecord(17));
  }
}

TEST_P(DarwinFileSystemTest,
       HeldRemovedDirectoryAndReplacementHaveSeparateSnapshots) {
  Options = darwin_test::createdDirectoryFileSystemOptions();
  Options->Directories.insert("/dir");
  Options->Metadata["/dir"] = darwin_test::modeMetadata(0040700, 10);
  Options->RemovableDirectories.insert("/dir");
  Options->FileSystemObservations->Objects["/dir"] =
      darwin_test::fileSystemSnapshot();
  Options->FileSystemObservations->Objects["/dir"].Owner = 17;
  reset();
  const auto Old = open("/dir"), Dup = ok(ServiceKind::Dup, {Old});
  ok(ServiceKind::Rmdir, {path("/dir")});
  error(ServiceKind::Statfs64, {path("/dir"), 1}, 2);
  refused(ServiceKind::Fstat64, {Old, Base + Page * 2 + 8},
          diagnostic::DirectoryMutated);
  ok(ServiceKind::Mkdir, {path("/dir"), 0700});
  const auto New = open("/dir");
  EXPECT_EQ(capture(New, "/dir"), darwin_test::fileSystemRecord());
  for (auto FD : {Old, Dup}) {
    Record Expected;
    Expected.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, Expected));
    ok(ServiceKind::Fstatfs64, {FD, Base + Page + 16});
    llvm::cantFail(Space->read(Base + Page, Expected));
    EXPECT_EQ(Expected, darwin_test::directoryFileSystemRecord(17));
  }
  ok(ServiceKind::Close, {Old});
  error(ServiceKind::Fstatfs64, {Old, 1}, 9);
  ok(ServiceKind::Close, {Dup});
  EXPECT_EQ(capture(New, "/dir"), darwin_test::fileSystemRecord());
}

TEST_P(DarwinFileSystemTest,
       CreatedDirectorySnapshotsDoNotGrantAttributeMutations) {
  Options = darwin_test::createdDirectoryFileSystemOptions(true, true);
  Options->ModeMutations.emplace();
  Options->OwnershipMutations.emplace();
  Options->TimestampMutations.emplace();
  reset();
  ok(ServiceKind::Mkdir, {path("/dir"), 0700});
  const auto Dir = open("/dir");
  const auto Before = status(Dir);
  refused(ServiceKind::Fchmod, {Dir, 0700}, diagnostic::FileModeProvider);
  refused(ServiceKind::Fchown, {Dir, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipProvider);
  std::array<uint8_t, 32> Times{};
  llvm::cantFail(Space->write(Base + 256, Times));
  refused(ServiceKind::Futimes, {Dir, Base + 256},
          diagnostic::FileTimestampProvider);
  EXPECT_EQ(capture(Dir, "/dir"), darwin_test::fileSystemRecord());
  EXPECT_EQ(status(Dir), Before);
  Options->OwnershipMutations.emplace().CreatedDirectoryObjects = true;
  Credentials->GroupAccessList = {20, 31};
  reset();
  ok(ServiceKind::Mkdir, {path("/dir"), 0700});
  const auto Changed = open("/dir");
  ok(ServiceKind::Fchown, {Changed, UINT32_MAX, 31});
  EXPECT_EQ(capture(Changed, "/dir"), darwin_test::fileSystemRecord());
  const auto Leaf = open("/dir/leaf", 0x202);
  EXPECT_EQ(capture(Leaf, "/dir/leaf"), darwin_test::fileSystemRecord());
  const auto ChildStatus = status(Leaf);
  EXPECT_EQ(ChildStatus[28], 31);
  EXPECT_EQ(ChildStatus[29], 0);
  EXPECT_EQ(ChildStatus[30], 0);
  EXPECT_EQ(ChildStatus[31], 0);
}

TEST_P(DarwinFileSystemTest,
       SnapshotSurvivesInvalidStatWithoutRepairingPathSearch) {
  Options = darwin_test::createdDirectoryFileSystemOptions();
  Options->Directories.insert("/dir");
  Options->Metadata["/dir"] = darwin_test::modeMetadata(0040700, 10);
  Options->MutableDirectories.insert("/dir");
  Options->FileSystemObservations->Objects["/dir"] =
      darwin_test::fileSystemSnapshot();
  reset();
  const auto Parent = open("/dir");
  ok(ServiceKind::Mkdir, {path("/dir/sub"), 0700});
  refused(ServiceKind::Fstat64, {Parent, Base + Page * 2 + 8},
          diagnostic::DirectoryMutated);
  Record B;
  B.fill(0xa5);
  llvm::cantFail(Space->write(Base + Page, B));
  ok(ServiceKind::Fstatfs64, {Parent, Base + Page + 16});
  llvm::cantFail(Space->read(Base + Page, B));
  EXPECT_EQ(B, darwin_test::fileSystemRecord());
  refused(ServiceKind::Statfs64, {path("/dir/sub"), 1},
          diagnostic::DirectoryMutated);
  refused(ServiceKind::Fstat64, {Parent, Base + Page * 2 + 8},
          diagnostic::DirectoryMutated);
}

TEST_P(DarwinFileSystemTest,
       CreatedDirectoryQueriesPreserveFullStatAndDescriptionFlags) {
  Options = darwin_test::createdDirectoryFileSystemOptions();
  reset();
  error(ServiceKind::Mkdir, {path("/"), 0700}, 17);
  error(ServiceKind::Mkdir, {1, 0700}, 14);
  ok(ServiceKind::Mkdir, {path("/dir"), 0700});
  const auto Dir = open("/dir"), Dup = ok(ServiceKind::Dup, {Dir});
  const auto Before = status(Dir);
  ok(ServiceKind::Fcntl, {Dup, 2, 1});
  const auto Flags = ok(ServiceKind::Fcntl, {Dup, 3});
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    EXPECT_EQ(capture(Dup, "/dir", High), darwin_test::fileSystemRecord());
    EXPECT_EQ(status(Dir), Before);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 3}), Flags);
  }
  error(route(), args(Dir, 1, "/dir"), 14);
  EXPECT_EQ(status(Dir), Before);
}

TEST_P(DarwinFileSystemTest,
       FailedDirectoryCreationCannotConsumeNextInodeOrBinding) {
  Options = darwin_test::createdDirectoryFileSystemOptions();
  Options->CreationPolicy->FirstInode = UINT64_MAX;
  reset();
  error(ServiceKind::Mkdir, {path("/"), 0700}, 17);
  error(ServiceKind::Mkdir, {1, 0700}, 14);
  ok(ServiceKind::Mkdir, {path("/dir"), 0700});
  const auto Dir = open("/dir");
  EXPECT_EQ(capture(Dir, "/dir"), darwin_test::fileSystemRecord());
  const auto Before = status(Dir);
  for (unsigned I = 16; I != 24; ++I)
    EXPECT_EQ(Before[I], 0xff);
  refused(ServiceKind::Mkdir, {path("/next"), 0700},
          diagnostic::FileCreationInode);
  error(ServiceKind::Statfs64, {path("/next"), 1}, 2);
  EXPECT_EQ(capture(Dir, "/dir"), darwin_test::fileSystemRecord());
  EXPECT_EQ(status(Dir), Before);
}

TEST(DarwinFileSystemOptions, SymbolicCreationRequiresIndependentNamespace) {
  auto O = darwin_test::fileSystemOptions();
  EXPECT_FALSE(O.FileSystemObservations->CreatedSymbolicLinkObjects);
  O.FileSystemObservations->CreatedSymbolicLinkObjects = true;
  auto E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
  O = darwin_test::symbolicLinkFileSystemOptions(true);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.CreationPolicy->Namespace.reset();
  E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
  O.FileSystemObservations->CreatedSymbolicLinkObjects = false;
  EXPECT_FALSE(bool(validateFileOptions(O)));
}
TEST(DarwinFileSystemOptions,
     InitialSymbolicSnapshotsShareExactReferenceBudget) {
  auto O = darwin_test::fileSystemOptions();
  O.Metadata.clear();
  O.SymbolicLinks["/link"] = {255};
  O.FileSystemObservations->Objects["/link"] = {};
  uint64_t Low = 0, High = darwin_file_limits::Bytes;
  while (Low < High) {
    const auto Mid = (Low + High + 1) / 2;
    O.Files["/data"].resize(Mid);
    auto E = validateFileOptions(O);
    const bool Good = !E;
    llvm::consumeError(std::move(E));
    if (Good)
      Low = Mid;
    else
      High = Mid - 1;
  }
  O.Files["/data"].resize(Low);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.FileSystemObservations->Objects.erase("/link");
  O.Files["/data"].resize(Low + 2168 + 6);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  auto E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileOptionsLimit);
}
TEST_P(DarwinFileSystemTest,
       SymbolicDescriptorAndFollowedTargetSnapshotsAreSeparate) {
  Options = darwin_test::symbolicLinkFileSystemOptions();
  Options->FileSystemObservations->Objects["/link"].Owner = 17;
  reset();
  const auto FD = open("/link", 0x200000);
  EXPECT_EQ(captureLink(FD), darwin_test::directoryFileSystemRecord(17));
  EXPECT_EQ(capture(FD, "/link"),
            FDRoute ? darwin_test::directoryFileSystemRecord(17)
                    : darwin_test::fileSystemRecord());
  Options->FileSystemObservations->Objects.erase("/link");
  reset();
  const auto Unbound = open("/link", 0x200000);
  refused(ServiceKind::Fstatfs64, {Unbound, 1}, diagnostic::FileSystemProvider);
  EXPECT_EQ(ok(ServiceKind::Statfs64, {path("/link"), Base + Page + 16}), 0u);
}
TEST_P(DarwinFileSystemTest,
       DanglingCyclicAndOpaqueTargetsNeedNoTargetMetadata) {
  Options = darwin_test::fileSystemOptions();
  const std::pair<const char *, std::vector<uint8_t>> Links[] = {
      {"/dang", {'m', 'i', 's', 's'}},
      {"/cycle", {'c', 'y', 'c', 'l', 'e'}},
      {"/raw", {255}}};
  for (unsigned I = 0; I != 3; ++I) {
    Options->SymbolicLinks[Links[I].first] = Links[I].second;
    Options->FileSystemObservations->Objects[Links[I].first] =
        darwin_test::fileSystemSnapshot();
    Options->FileSystemObservations->Objects[Links[I].first].Owner = 17 + I;
  }
  reset();
  for (unsigned I = 0; I != 3; ++I) {
    const auto FD = open(Links[I].first, 0x200000);
    EXPECT_EQ(captureLink(FD), darwin_test::directoryFileSystemRecord(17 + I));
    refused(ServiceKind::Fstat64, {FD, Base + Page * 2 + 8},
            diagnostic::FileMetadata);
    EXPECT_EQ(captureLink(FD), darwin_test::directoryFileSystemRecord(17 + I));
  }
  error(ServiceKind::Statfs64, {path("/dang"), 1}, 2);
  error(ServiceKind::Statfs64, {path("/cycle"), 1}, 62);
}
TEST_P(DarwinFileSystemTest,
       SymbolicQueriesPreserveFullStatFlagsAndTargetBytes) {
  Options = darwin_test::symbolicLinkFileSystemOptions();
  reset();
  const auto FD = open("/link", 0x200000), Copy = ok(ServiceKind::Dup, {FD});
  const auto Before = status(FD);
  ok(ServiceKind::Fcntl, {Copy, 2, 1});
  const auto Flags = ok(ServiceKind::Fcntl, {Copy, 3});
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    EXPECT_EQ(captureLink(High | Copy), darwin_test::fileSystemRecord());
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Copy, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Copy, 3}), Flags);
  }
  std::array<uint8_t, 4> Target{};
  EXPECT_EQ(ok(ServiceKind::ReadLink, {path("/link"), Base + 256, 4}), 4u);
  llvm::cantFail(Space->read(Base + 256, Target));
  EXPECT_EQ(Target, (std::array<uint8_t, 4>{'d', 'a', 't', 'a'}));
  refused(ServiceKind::Fchown, {Copy, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipEnvironment);
  EXPECT_EQ(status(FD), Before);
}
TEST_P(DarwinFileSystemTest, HeldSymbolicAndReplacementNeverShareNameBindings) {
  for (bool Created : {false, true}) {
    Options = darwin_test::symbolicLinkFileSystemOptions();
    Options->FileSystemObservations->Objects["/link"].Owner = 17;
    Options->FileSystemObservations->CreatedSymbolicLinkObjects = Created;
    reset();
    const auto FD = open("/link", 0x200000), Copy = ok(ServiceKind::Dup, {FD});
    ok(ServiceKind::Rename, {path("/link"), path("/moved", Base + 128)});
    EXPECT_EQ(captureLink(Copy), darwin_test::directoryFileSystemRecord(17));
    ok(ServiceKind::Unlink, {path("/moved")});
    EXPECT_EQ(captureLink(FD), darwin_test::directoryFileSystemRecord(17));
    error(ServiceKind::Statfs64, {path("/moved"), 1}, 2);
    ok(ServiceKind::Symlink, {path("missing"), path("/link", Base + 128)});
    const auto New = open("/link", 0x200000);
    if (Created)
      EXPECT_EQ(captureLink(New), darwin_test::fileSystemRecord());
    else
      refused(ServiceKind::Fstatfs64, {New, 1}, diagnostic::FileSystemProvider);
    EXPECT_EQ(captureLink(Copy), darwin_test::directoryFileSystemRecord(17));
    error(ServiceKind::Statfs64, {path("/link"), 1}, 2);
    ok(ServiceKind::Close, {FD});
    error(ServiceKind::Fstatfs64, {FD, 1}, 9);
    EXPECT_EQ(captureLink(Copy), darwin_test::directoryFileSystemRecord(17));
  }
}
TEST_P(DarwinFileSystemTest,
       SymbolicRegularAndDirectoryCreationFlagsAreIndependent) {
  for (bool Symbolic : {false, true})
    for (bool Directory : {false, true})
      for (bool Regular : {false, true}) {
        Options = darwin_test::symbolicLinkFileSystemOptions(true);
        auto &F = *Options->FileSystemObservations;
        F.CreatedSymbolicLinkObjects = Symbolic;
        F.CreatedDirectoryObjects = Directory;
        F.CreatedRegularObjects = Regular;
        Options->SymbolicLinks["/initial"] = {'d', 'a', 't', 'a'};
        Options->MutableSymbolicLinks.insert("/initial");
        reset();
        refused(ServiceKind::Fstatfs64, {open("/initial", 0x200000), 1},
                diagnostic::FileSystemProvider);
        ok(ServiceKind::Symlink, {path("data"), path("/child", Base + 128)});
        const auto Link = open("/child", 0x200000);
        if (Symbolic)
          EXPECT_EQ(captureLink(Link), darwin_test::fileSystemRecord());
        else
          refused(ServiceKind::Fstatfs64, {Link, 1},
                  diagnostic::FileSystemProvider);
        ok(ServiceKind::Mkdir, {path("/dir"), 0700});
        ok(ServiceKind::Symlink,
           {path("../data"), path("/dir/link", Base + 128)});
        const auto Nested = open("/dir/link", 0x200000);
        if (Symbolic && Directory)
          EXPECT_EQ(captureLink(Nested), darwin_test::fileSystemRecord());
        else
          refused(ServiceKind::Fstatfs64, {Nested, 1},
                  diagnostic::FileSystemProvider);
        const auto Leaf = open("/dir/leaf", 0x202);
        if (Directory && Regular)
          EXPECT_EQ(captureLink(Leaf), darwin_test::fileSystemRecord());
        else
          refused(ServiceKind::Fstatfs64, {Leaf, 1},
                  diagnostic::FileSystemProvider);
      }
}
TEST_P(DarwinFileSystemTest, CreatedSymbolicUsesActualMovedAndExchangedParent) {
  for (bool Swap : {false, true}) {
    Options = darwin_test::symbolicLinkFileSystemOptions(true);
    Options->Directories = {"/", "/a", "/b"};
    for (const auto &V :
         {std::pair<const char *, uint64_t>{"/a", 10}, {"/b", 11}}) {
      Options->Metadata[V.first] = darwin_test::modeMetadata(0040700, V.second);
      Options->MutableDirectories.insert(V.first);
      Options->DirectoryMutationPolicies[V.first] = {16, {91, 92}};
    }
    Options->MovableDirectories.insert("/a");
    Options->ExchangeableDirectories = {"/a", "/b"};
    Options->SwapRenameDirectories.insert("/");
    Options->FileSystemObservations->Objects["/a"] =
        darwin_test::fileSystemSnapshot();
    Options->FileSystemObservations->Objects["/a"].Owner = 17;
    Options->FileSystemObservations->Objects["/b"] =
        darwin_test::fileSystemSnapshot();
    Options->FileSystemObservations->Objects["/b"].Owner = 23;
    reset();
    const auto From = path("/a"), To = path(Swap ? "/b" : "/moved", Base + 128);
    if (Swap)
      ok(ServiceKind::RenameAtX, {UINT64_MAX, From, UINT64_MAX, To, 2});
    else
      ok(ServiceKind::Rename, {From, To});
    const std::string Name = std::string(Swap ? "/b" : "/moved") + "/link";
    ok(ServiceKind::Symlink, {path("/data"), path(Name, Base + 128)});
    const auto FD = open(Name, 0x200000);
    EXPECT_EQ(captureLink(FD), darwin_test::directoryFileSystemRecord(17));
    EXPECT_EQ(capture(FD, Name),
              FDRoute ? darwin_test::directoryFileSystemRecord(17)
                      : darwin_test::fileSystemRecord());
    ok(ServiceKind::Rename, {path(Name), path("/link", Base + 128)});
    EXPECT_EQ(captureLink(FD), darwin_test::directoryFileSystemRecord(17));
  }
}
TEST_P(DarwinFileSystemTest, SymbolicSnapshotDoesNotRepairInvalidMetadata) {
  Options = darwin_test::symbolicLinkFileSystemOptions();
  Options->SymbolicLinkMutationPolicies.clear();
  reset();
  const auto FD = open("/link", 0x200000);
  ok(ServiceKind::Unlink, {path("/link")});
  refused(ServiceKind::Fstat64, {FD, Base + Page * 2 + 8},
          diagnostic::SymbolicLinkMutatedMetadata);
  EXPECT_EQ(captureLink(FD), darwin_test::fileSystemRecord());
  refused(ServiceKind::Fstat64, {FD, Base + Page * 2 + 8},
          diagnostic::SymbolicLinkMutatedMetadata);
}
TEST_P(DarwinFileSystemTest,
       CreatedSymbolicZeroUnknownAndFailedInodesStayExplicit) {
  for (bool Bound : {false, true}) {
    Options = darwin_test::symbolicLinkFileSystemOptions(true);
    Options->FileSystemObservations->Objects.erase("/");
    if (Bound)
      Options->FileSystemObservations->Objects["/"] = {};
    Options->CreationPolicy->FirstInode = UINT64_MAX;
    reset();
    error(ServiceKind::Symlink, {1, path("/child", Base + 128)}, 14);
    error(ServiceKind::Symlink, {path("data"), path("/data", Base + 128)}, 17);
    ok(ServiceKind::Symlink, {path("data"), path("/child", Base + 128)});
    const auto FD = open("/child", 0x200000);
    if (Bound) {
      Record Zero{};
      std::fill_n(Zero.begin(), 16, 0xa5);
      std::fill_n(Zero.end() - 16, 16, 0xa5);
      EXPECT_EQ(captureLink(FD), Zero);
      error(ServiceKind::Fstatfs64, {FD, 1}, 14);
    } else
      refused(ServiceKind::Fstatfs64, {FD, 1}, diagnostic::FileSystemProvider);
    refused(ServiceKind::Symlink, {path("data"), path("/next", Base + 128)},
            diagnostic::FileCreationInode);
    error(ServiceKind::Statfs64, {path("/next"), 1}, 2);
    if (Bound)
      EXPECT_EQ(captureLink(FD)[16], 0);
  }
}
TEST_P(DarwinFileSystemTest,
       SymbolicDescriptorCopyoutHasNoPartialEffectsOrStatPrerequisite) {
  Options = darwin_test::fileSystemOptions();
  Options->SymbolicLinks["/link"] = {'m', 'i', 's', 's'};
  Options->FileSystemObservations->Objects["/link"] =
      darwin_test::fileSystemSnapshot();
  Credentials.reset();
  reset();
  const auto FD = open("/link", 0x200000);
  EXPECT_EQ(captureLink(FD), darwin_test::fileSystemRecord());
  error(ServiceKind::Fstatfs64, {FD, 0}, 14);
  std::array<uint8_t, 32> Before, After;
  Before.fill(0x6c);
  llvm::cantFail(Space->write(Base + Page * 4 - 32, Before));
  refused(ServiceKind::Fstatfs64, {FD, Base + Page * 4 - 32},
          diagnostic::FileSystemPartialOutput);
  llvm::cantFail(Space->read(Base + Page * 4 - 32, After));
  EXPECT_EQ(After, Before);
  refused(ServiceKind::Fstat64, {FD, Base + Page * 2 + 8},
          diagnostic::FileMetadata);
}

class DarwinMountEnumerationTest : public DarwinFileSystemTest {
protected:
  static constexpr auto K = ServiceKind::Getfsstat64;
  using Buffer = std::array<uint8_t, 3 * 2168 + 32>;
  void SetUp() override {
    DarwinFileSystemTest::SetUp();
    const auto S = darwin_test::fileSystemSnapshot();
    Options->FileSystemEnumeration.emplace().Mounts = {
        {S, DarwinFileSystemSnapshot{}},
        {DarwinFileSystemSnapshot{}, S},
        {S, DarwinFileSystemSnapshot{}}};
    reset();
  }
  uint64_t output() { return Base + Page + 16; }
  void clear() {
    Buffer B;
    B.fill(0xa5);
    llvm::cantFail(Space->write(output() - 16, B));
  }
  Buffer bytes() {
    Buffer B;
    llvm::cantFail(Space->read(output() - 16, B));
    return B;
  }
  Buffer expected(std::initializer_list<bool> Records) {
    Buffer B;
    B.fill(0xa5);
    const auto S = darwin_test::fileSystemRecord();
    auto Out = B.begin() + 16;
    for (bool Nonzero : Records) {
      if (Nonzero)
        std::copy(S.begin() + 16, S.end() - 16, Out);
      else
        std::fill_n(Out, 2168, 0);
      Out += 2168;
    }
    return B;
  }
  void cache(std::initializer_list<bool> Records) {
    clear();
    EXPECT_EQ(ok(K, {output(), 6504, 2}), Records.size());
    EXPECT_EQ(bytes(), expected(Records));
  }
};
TEST_P(DarwinMountEnumerationTest, UnknownAndEmptyTablesRemainDistinct) {
  Options->FileSystemEnumeration.reset();
  reset();
  refused(K, {0, 0, 2}, diagnostic::FileSystemEnumerationEnvironment);
  refused(K, {1, 0, 2}, diagnostic::FileSystemEnumerationEnvironment);
  Options.reset();
  reset();
  refused(K, {0, 0, 2}, diagnostic::FileSystemEnumerationEnvironment);
  Options.emplace().FileSystemEnumeration.emplace();
  reset();
  EXPECT_EQ(ok(K, {0, UINT64_MAX, UINT64_MAX}), 0u);
  EXPECT_EQ(ok(K, {UINT64_MAX, UINT64_MAX, UINT64_MAX}), 0u);
}
TEST_P(DarwinMountEnumerationTest, CountsAndShortBuffersNeedNoRefreshOrAccess) {
  for (auto &M : Options->FileSystemEnumeration->Mounts)
    M.Refreshed.reset();
  reset();
  for (uint64_t Size : std::array<uint64_t, 7>{0, 1, 2167, 2168, 0x80000000,
                                               0xffffffff, UINT64_MAX})
    for (uint64_t Flags : std::array<uint64_t, 5>{0, 1, 2, 4, UINT64_MAX})
      EXPECT_EQ(ok(K, {0, Size, Flags}), 3u);
  for (uint64_t Size : {0ULL, 1ULL, 2167ULL, 0x1234567800000000ULL})
    EXPECT_EQ(ok(K, {UINT64_MAX, Size, 0}), 0u);
  cache({true, false, true});
  refused(K, {1, 2168, 0}, diagnostic::FileSystemRefresh);
}
TEST_P(DarwinMountEnumerationTest,
       OrderedDuplicatesSignedCapacityAndHighCarriers) {
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
    for (const auto &[Size, Count] :
         std::vector<std::pair<uint32_t, size_t>>{{0, 0},
                                                  {2167, 0},
                                                  {2168, 1},
                                                  {4335, 1},
                                                  {4336, 2},
                                                  {6504, 3},
                                                  {INT32_MAX, 3},
                                                  {0x80000000u, 3},
                                                  {UINT32_MAX, 3}}) {
      clear();
      EXPECT_EQ(ok(K, {output(), High | Size, High | 2}), Count);
      EXPECT_EQ(bytes(), Count == 0   ? expected({})
                         : Count == 1 ? expected({true})
                         : Count == 2 ? expected({true, false})
                                      : expected({true, false, true}));
    }
}
TEST_P(DarwinMountEnumerationTest, RefreshFlagPrecedenceCacheAndProcessReset) {
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
    for (uint32_t Flags :
         {0u, 1u, 2u, 3u, 4u, 6u, 7u, 0x80000000u, 0x80000002u, UINT32_MAX}) {
      reset();
      clear();
      EXPECT_EQ(ok(K, {output(), 6504, High | Flags}), 3u);
      const bool Cached = Flags == 2 || Flags == 0x80000002u;
      EXPECT_EQ(bytes(), Cached ? expected({true, false, true})
                                : expected({false, true, false}));
      if (Cached)
        cache({true, false, true});
      else
        cache({false, true, false});
      EXPECT_EQ(Options->FileSystemEnumeration->Mounts[0].Cached.BlockSize,
                0x10203040u);
    }
  reset();
  cache({true, false, true});
}
TEST_P(DarwinMountEnumerationTest, TruncatedRefreshTouchesOnlyCopiedMounts) {
  clear();
  EXPECT_EQ(ok(K, {output(), 4335, 1}), 1u);
  EXPECT_EQ(bytes(), expected({false}));
  cache({false, false, true});
}
TEST_P(DarwinMountEnumerationTest, MissingLaterRefreshRetainsEarlierEffects) {
  Options->FileSystemEnumeration->Mounts[1].Refreshed.reset();
  reset();
  clear();
  refused(K, {output(), 6504, 1}, diagnostic::FileSystemRefresh);
  EXPECT_EQ(bytes(), expected({false}));
  cache({false, false, true});
}
TEST_P(DarwinMountEnumerationTest, CompleteFaultRefreshesCurrentMountFirst) {
  for (uint64_t Address : {uint64_t(1), value::UserLimit, UINT64_MAX}) {
    reset();
    error(K, {Address, 6504, 1}, 14);
    cache({false, false, true});
  }
}
TEST_P(DarwinMountEnumerationTest, LaterFaultPreservesCopiesAndRefreshes) {
  const auto Address = Base + Page * 4 - 2168;
  std::array<uint8_t, 2168> B;
  B.fill(0xa5);
  llvm::cantFail(Space->write(Address, B));
  error(K, {Address, 6504, 1}, 14);
  llvm::cantFail(Space->read(Address, B));
  EXPECT_EQ(B, (std::array<uint8_t, 2168>{}));
  cache({false, true, true});
}
TEST_P(DarwinMountEnumerationTest, PartialRecordStopsBeforeItsEffects) {
  for (bool Later : {false, true}) {
    reset();
    const size_t Size = 128 + (Later ? 2168 : 0);
    const auto Address = Base + Page * 4 - Size;
    std::vector<uint8_t> B(Size, 0xa5), Expected = B;
    llvm::cantFail(Space->write(Address, B));
    refused(K, {Address, 6504, 1}, diagnostic::FileSystemPartialOutput);
    llvm::cantFail(Space->read(Address, B));
    if (Later)
      std::fill_n(Expected.begin(), 2168, 0);
    EXPECT_EQ(B, Expected);
    if (Later)
      cache({false, false, true});
    else
      cache({true, false, true});
  }
}
TEST_P(DarwinMountEnumerationTest, ObjectSnapshotsRemainIndependent) {
  const auto FD = open();
  clear();
  EXPECT_EQ(ok(K, {output(), 6504, 1}), 3u);
  EXPECT_EQ(capture(FD), darwin_test::fileSystemRecord());
  FDRoute = true;
  EXPECT_EQ(capture(FD), darwin_test::fileSystemRecord());
  cache({false, true, false});
}
TEST(DarwinMountEnumerationOptions, EntryAndSnapshotBytesShareExistingLimits) {
  DarwinFileOptions O;
  auto &Mounts = O.FileSystemEnumeration.emplace().Mounts;
  Mounts.resize(256);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  Mounts.emplace_back();
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  Mounts.pop_back();
  O.Files["/data"] = {};
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  Mounts.resize(1);
  Mounts[0].Refreshed.emplace();
  O.Files["/data"].resize(darwin_file_limits::Bytes - 2 * 2168 - 6);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  Mounts[0].Refreshed.reset();
  O.Files["/data"].resize(darwin_file_limits::Bytes - 2168 - 6);
  EXPECT_FALSE(bool(validateFileOptions(O)));
}
TEST(DarwinMountEnumerationOptions,
     BothSnapshotsUseStrictNamesAndQueryIsolation) {
  for (bool Refreshed : {false, true})
    for (unsigned I = 0; I != 5; ++I) {
      DarwinFileOptions O;
      auto &M = O.FileSystemEnumeration.emplace().Mounts.emplace_back();
      M.Refreshed.emplace();
      auto &S = Refreshed ? *M.Refreshed : M.Cached;
      switch (I) {
      case 0:
        S.TypeName = std::string(16, 'x');
        break;
      case 1:
        S.MountPoint = std::string(1024, 'x');
        break;
      case 2:
        S.MountedFrom = std::string(1024, 'x');
        break;
      case 3:
        S.TypeName = std::string("a\0b", 3);
        break;
      case 4:
        S.MountPoint = std::string(1, char(0xff));
        break;
      }
      auto E = validateFileOptions(O);
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    }
  for (auto Domain : {DarwinFileAuthorization::StaticOwnerQueries,
                      DarwinFileAuthorization::StaticOrdinaryQueries}) {
    DarwinFileOptions O;
    O.Authorization = Domain;
    O.FileSystemEnumeration.emplace();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
INSTANTIATE_TEST_SUITE_P(Page, DarwinMountEnumerationTest,
                         testing::Values(std::make_tuple(uint64_t(4096), false),
                                         std::make_tuple(uint64_t(16384),
                                                         false)));

INSTANTIATE_TEST_SUITE_P(PageAndRoute, DarwinFileSystemTest,
                         testing::Combine(testing::Values(uint64_t(4096),
                                                          uint64_t(16384)),
                                          testing::Bool()));
} // namespace
} // namespace neverd::emulation::darwin_model
