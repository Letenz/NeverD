//===- DarwinModeTests.cpp - Ordinary current-object mode contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinModeTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
class DarwinModeTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only mode"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::modeOptions();
    Credentials = darwin_test::modeCredentials();
    reset();
  }
  void reset(uint32_t ObservedCreationUID = 777) {
    if (Options) {
      auto Error = validateFileOptions(*Options);
      ASSERT_FALSE(bool(Error))
          << (Error ? llvm::toString(std::move(Error)) : "");
    }
    // A legacy scalar observation never establishes mode authority.
    Files =
        std::make_unique<DarwinFiles>(*Space, Options, process_defaults::Output,
                                      ObservedCreationUID, Credentials);
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
                      ExecutionBackendKind::Unicorn, "memory-only mode"};
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
  Record changed(Record B, uint16_t Mode, int64_t Seconds = 71,
                 int64_t Nanoseconds = 72) {
    // Literal stat64 offsets; no production serializer is used as an oracle.
    llvm::support::endian::write16le(B.data() + 12, Mode);
    llvm::support::endian::write64le(B.data() + 72, Seconds);
    llvm::support::endian::write64le(B.data() + 80, Nanoseconds);
    return B;
  }
  std::array<uint64_t, 6> operands(ServiceKind K, uint64_t FD, uint64_t Mode,
                                   llvm::StringRef Name = "/data") {
    if (K == ServiceKind::Fchmod)
      return {FD, Mode};
    if (K == ServiceKind::FchmodAt)
      return {UINT64_MAX, path(Name), Mode, 0};
    return {path(Name), Mode};
  }
  void mutationPolicies() {
    Options->WritableFiles.insert("/data");
    Options->MutationPolicies["/data"] = {512, {81, 82}};
    Options->MutableDirectories.insert("/");
    Options->DirectoryMutationPolicies["/"] = {16, {91, 92}};
    Options->InitialUmask = 022;
    DarwinFileCreationPolicy P;
    P.FirstInode = 100;
    P.BlockSize = 512;
    P.Time = {41, 42};
    P.Mutation = {512, {81, 82}};
    Options->CreationPolicy = P;
    reset();
  }
};

TEST(DarwinModeOptions, RequiresCompleteInitialRegularUniqueGrants) {
  const auto Original = darwin_test::modeOptions();
  for (unsigned I = 0; I != 9; ++I) {
    auto O = Original;
    switch (I) {
    case 0:
      O.ModeMutations->Objects["/missing"] = {1, 2};
      break;
    case 1:
      O.Metadata.erase("/");
      O.ModeMutations->Objects["/"] = {1, 2};
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
      O.ModeMutations->Objects["/data"].Nanoseconds = -1;
      break;
    case 7:
      O.ModeMutations->Objects["/data"].Nanoseconds = 1000000000;
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
    O.ModeMutations->Objects.clear();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}

TEST(DarwinModeOptions, ObjectReferencesShareTheExistingByteOwner) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 6);
  O.Metadata["/a"] =
      darwin_test::modeMetadata(0100000, 1, O.Files["/a"].size());
  O.ModeMutations.emplace().Objects["/a"] = {INT64_MIN, 999999999};
  EXPECT_FALSE(bool(validateFileOptions(O))); // two path+NUL references
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files["/a"].pop_back();
  --O.Metadata["/a"].Size;
  O.ModeMutations->Objects.clear();
  EXPECT_FALSE(bool(validateFileOptions(O))); // an empty environment is valid
}

TEST_P(DarwinModeTest, TypedCarriersOwnerModeZeroAndCurrentMetadata) {
  const auto FD = open();
  const auto Before = status(FD);
  for (auto K :
       {ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt})
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL})
      for (uint16_t Mode : {0000, 0640, 0604, 0600}) {
        EXPECT_EQ(ok(K, operands(K, High | FD, High | 0x7fff0000 | Mode)), 0u);
        EXPECT_EQ(status(FD), changed(Before, 0100000 | Mode));
      }
  EXPECT_EQ(Options->Metadata.at("/data").Mode, 0100600);
  EXPECT_EQ(Options->Metadata.at("/data").ChangeTime.Seconds, 5);
  EXPECT_EQ(Options->ModeMutations->Objects.at("/data").Nanoseconds, 72);
}

TEST_P(DarwinModeTest, NonownerSameModeReachesProviderChangedModeIsEPERM) {
  Options->Metadata["/data"].UID = 700;
  reset();
  const auto FD = open();
  const auto Before = status(FD);
  auto Current = Before;
  for (auto K :
       {ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt}) {
    error(K, operands(K, FD, 0400), 1);
    EXPECT_EQ(status(FD), Current);
    EXPECT_EQ(ok(K, operands(K, FD, 0x123456787fff0180ULL)), 0u); // 0600
    Current = changed(Before, 0100600);
    EXPECT_EQ(status(FD), Current);
    error(K, operands(K, FD, 0000), 1);
    EXPECT_EQ(status(FD), changed(Before, 0100600));
  }
  Options->ModeMutations->Objects.clear();
  reset();
  const auto Ungranted = open();
  for (auto K :
       {ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt}) {
    error(K, operands(K, Ungranted, 0400), 1);
    refused(K, operands(K, Ungranted, 0600), diagnostic::FileModeProvider);
  }
}

TEST_P(DarwinModeTest, CredentialsMustBeExplicitEffectiveAndNonroot) {
  Credentials->RealUID =
      0; // the real subject must not authorize this operation
  reset();
  const auto FD = open();
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0000}), 0u);
  Credentials.reset();
  reset();
  refused(ServiceKind::Fchmod, {open(), 0000},
          diagnostic::FileOrdinaryAuthorizationCredentials);
  Credentials = darwin_test::modeCredentials();
  Credentials->EffectiveUID = 0;
  reset();
  refused(ServiceKind::Fchmod, {open(), 0000},
          diagnostic::FileOrdinaryAuthorizationSubject);
}

TEST_P(DarwinModeTest, UnsupportedCurrentSecurityAttributesStayUnknown) {
  Options->ModeMutations->Objects.clear();
  Options->Metadata["/data"].Flags = 1;
  reset();
  refused(ServiceKind::Fchmod, {open(), 0600},
          diagnostic::FileModeSecurityAttributes);
  Options->Metadata["/data"].Flags = 0;
  Options->Metadata["/data"].Mode |= 04000;
  reset();
  refused(ServiceKind::Fchmod, {open(), 0600},
          diagnostic::FileModeSecurityAttributes);
  Options = darwin_test::modeOptions();
  Options->Metadata["/"].Flags = 1;
  reset();
  refused(ServiceKind::Chmod, {path("/data"), 0600},
          diagnostic::FileModeSecurityAttributes);
}

TEST_P(DarwinModeTest, EmptyDeclarationDoesNotProveMissingNamesBehindDenial) {
  Options->ModeMutations->Objects.clear();
  Options->Metadata["/"].Mode = 0040000;
  reset();
  error(ServiceKind::Chmod, {path("/missing"), 0600}, 13);
  Options->Metadata.erase("/");
  reset();
  refused(ServiceKind::Chmod, {path("/missing"), 0600},
          diagnostic::FileMetadata);
  Options = darwin_test::modeOptions();
  Options->ModeMutations->Objects.clear();
  Credentials.reset();
  reset();
  refused(ServiceKind::Chmod, {path("/missing"), 0600},
          diagnostic::FileOrdinaryAuthorizationCredentials);
}

TEST_P(DarwinModeTest,
       AbsentEmptyAndMissingCatalogueRetainIndependentPreflight) {
  for (unsigned State = 0; State != 3; ++State) {
    Options = darwin_test::modeOptions();
    if (State == 0)
      Options->ModeMutations.reset();
    if (State == 1)
      Options->ModeMutations->Objects.clear();
    if (State == 2)
      Options.reset();
    reset();
    error(ServiceKind::Chmod, {UINT64_MAX, 07000}, 14);
    error(ServiceKind::Fchmod, {UINT64_MAX, 07000}, 9);
    error(ServiceKind::FchmodAt, {UINT64_MAX, UINT64_MAX, 07000, 8}, 22);
    error(ServiceKind::FchmodAt, {UINT64_MAX, path("data"), 07000, 0}, 9);
    error(ServiceKind::Chmod, {path(""), 07000}, 2);
    error(ServiceKind::FchmodAt, {uint32_t(-2), path(""), 07000, 0}, 2);
    if (State == 1) {
      error(ServiceKind::Chmod, {path("/missing"), 0600}, 2);
      refused(ServiceKind::FchmodAt, {UINT64_MAX, path("/data"), 0600, 0},
              diagnostic::FileModeProvider);
    } else {
      refused(ServiceKind::Chmod, {path("/missing"), 0600},
              diagnostic::FileModeEnvironment);
    }
    if (Options) {
      refused(ServiceKind::Fchmod, {open(), 0600},
              State == 1 ? diagnostic::FileModeProvider
                         : diagnostic::FileModeEnvironment);
    }
  }
}

TEST_P(DarwinModeTest, FlagsAndPrefixErrorsPrecedeSpecialModeAndGuestWrites) {
  const auto FD = open();
  const auto Before = status(FD);
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    error(ServiceKind::FchmodAt, {UINT64_MAX, UINT64_MAX, 07000, High | 8}, 22);
    error(ServiceKind::FchmodAt, {UINT64_MAX, UINT64_MAX, 07000, High}, 14);
    error(ServiceKind::FchmodAt, {FD, path("data"), 07000, High}, 20);
    EXPECT_EQ(ok(ServiceKind::FchmodAt,
                 {UINT64_MAX, path("/data"), 0600, High | 0x820}),
              0u);
  }
  for (auto K :
       {ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt})
    refused(K, operands(K, FD, 07000), diagnostic::FileModePermissions);
  EXPECT_EQ(status(FD), changed(Before, 0100600));
  llvm::cantFail(Space->protect(Base, Page, Read | UserAccessible));
  EXPECT_EQ(ok(ServiceKind::Chmod, {Base, 0604}), 0u);
  EXPECT_EQ(status(FD), changed(Before, 0100604));
}

TEST_P(DarwinModeTest, SearchUsesActualEffectiveParentBeforeMissingOrDots) {
  Options->Directories.insert("/locked");
  Options->Metadata["/locked"] = darwin_test::modeMetadata(0040000, 3);
  reset();
  for (llvm::StringRef Name :
       {"/locked/missing", "/locked/../data", "/locked/./missing"})
    error(ServiceKind::Chmod, {path(Name), 0600}, 13);
  error(ServiceKind::FchmodAt, {open("/locked"), path("missing"), 0600, 0}, 13);
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/./data"), 0604}), 0u);
  Options->Metadata["/locked"].Mode = 0040700;
  reset();
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/locked/../data"), 0604}), 0u);
  Options->Metadata.erase("/locked");
  reset();
  refused(ServiceKind::Chmod, {path("/locked/missing"), 0600},
          diagnostic::FileMetadata);
}

TEST_P(DarwinModeTest, SearchRetainsWholeMaskGroupKnowledgeWithoutRealCopy) {
  auto &M = Options->Metadata["/"];
  M.Mode = 0040010;
  M.UID = 700;
  M.GID = 40;
  reset();
  refused(ServiceKind::Chmod, {path("/data"), 0600},
          diagnostic::FileOrdinaryAuthorizationGroups);
  Credentials->GroupAccessList = {20, 40};
  reset();
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/data"), 0604}), 0u);
  Credentials->GroupAccessList = {20};
  reset();
  refused(ServiceKind::Chmod, {path("/data"), 0600},
          diagnostic::FileOrdinaryAuthorizationGroups);
  Credentials->GroupMembershipUID = 4294967195u;
  reset();
  error(ServiceKind::Chmod, {path("/data"), 0600}, 13);
  M.Mode = 0040011; // agreement does not require an external group resolver
  Credentials->GroupAccessList.reset();
  Credentials->GroupMembershipUID.reset();
  reset();
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/data"), 0604}), 0u);
}

TEST_P(DarwinModeTest, SearchNeverUsesInvalidatedDirectoryObservation) {
  Options->MutableDirectories.insert("/");
  reset();
  const auto FD = open();
  const auto Source = path("/data");
  const auto Target = path("/renamed", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Rename, {Source, Target}), 0u);
  refused(ServiceKind::Chmod, {path("/renamed"), 0604},
          diagnostic::DirectoryMutated);
  refused(ServiceKind::Chmod, {path("/missing"), 0604},
          diagnostic::DirectoryMutated);
  refused(ServiceKind::Fchmod, {FD, 0604}, diagnostic::FileMutatedMetadata);
}

TEST_P(DarwinModeTest, LinksFollowActualSearchAndFinalUnsupportedKinds) {
  Options->SymbolicLinks["/alias"] = {'d', 'a', 't', 'a'};
  Options->SymbolicLinks["/via"] = {'/'};
  reset();
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/alias"), 0604}), 0u);
  for (uint32_t Flags : {0x20u, 0x800u, 0x820u})
    refused(ServiceKind::FchmodAt, {UINT64_MAX, path("/alias"), 0604, Flags},
            diagnostic::FileModeTarget);
  error(ServiceKind::FchmodAt, {UINT64_MAX, path("/via/data"), 0604, 0x800},
        62);
  EXPECT_EQ(
      ok(ServiceKind::FchmodAt, {UINT64_MAX, path("/via/data"), 0600, 0x20}),
      0u);
  refused(ServiceKind::Chmod, {path("/"), 0604}, diagnostic::FileModeProvider);
  refused(ServiceKind::Fchmod, {open("/"), 0604}, diagnostic::FileModeProvider);
  refused(ServiceKind::Fchmod, {1, 0604}, diagnostic::FileModeTarget);
}

TEST_P(DarwinModeTest, ReadonlyDupAndIndependentOpensShareOnlyObjectMetadata) {
  const auto FD = open(), Copy = ok(ServiceKind::Dup, {FD}), Separate = open();
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Copy, 0000}), 0u);
  for (auto F : {FD, Copy, Separate})
    EXPECT_EQ(status(F), changed(Before, 0100000));
  EXPECT_EQ(ok(ServiceKind::Lseek, {Copy, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Separate, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {Separate, Base + Page * 2, 6}), 6u);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Base + Page * 2, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Fchmod, {FD, 07000}, 9);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Copy, 0604}), 0u);
  EXPECT_EQ(status(Separate), changed(Before, 0100604));
}

TEST_P(DarwinModeTest, DataPoliciesPreserveCurrentModeAndRemainIndependent) {
  mutationPolicies();
  const auto FD = open("/data", 2);
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0000}), 0u);
  llvm::cantFail(Space->writeInteger(Base + Page * 2, 'x', 1));
  EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + Page * 2, 1, 0}), 1u);
  auto Written = changed(Before, 0100000, 81, 82);
  llvm::support::endian::write64le(Written.data() + 56, 81);
  llvm::support::endian::write64le(Written.data() + 64, 82);
  EXPECT_EQ(status(FD), Written);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0604}), 0u);
  EXPECT_EQ(status(FD), changed(Written, 0100604));
  Options->MutationPolicies.clear();
  reset();
  const auto Unknown = open("/data", 2);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {Unknown, Base + Page * 2, 1, 0}), 1u);
  refused(ServiceKind::Fchmod, {Unknown, 0604},
          diagnostic::FileMutatedMetadata);
}

TEST_P(DarwinModeTest, XattrMutationCannotRestoreModeAuthorityOrCompleteStat) {
  Options->ExtendedAttributes["/data"] = {{"user.mode", {5}}};
  Options->MutableExtendedAttributes.insert("/data");
  reset();
  const auto FD = open();
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0604}), 0u);
  EXPECT_EQ(ok(ServiceKind::FgetXattr,
               {FD, path("user.mode", Base + 128), Base + Page * 2, 8}),
            1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page * 2, 1)), 5u);
  EXPECT_EQ(status(FD), changed(Before, 0100604));
  llvm::cantFail(Space->writeInteger(Base + Page * 2, 6, 1));
  EXPECT_EQ(ok(ServiceKind::FsetXattr, {FD, Base + 128, Base + Page * 2, 1}),
            0u);
  refused(ServiceKind::Fchmod, {FD, 0600}, diagnostic::FileMutatedMetadata);
}

TEST_P(DarwinModeTest, HardlinksRenamesHeldUnlinkAndNameReuseRetainGrants) {
  mutationPolicies();
  const auto FD = open();
  EXPECT_EQ(ok(ServiceKind::Link, {path("/data"), path("/alias", Base + 128)}),
            0u);
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/alias"), 0604}), 0u);
  auto Linked = status(FD);
  EXPECT_EQ(llvm::support::endian::read16le(Linked.data() + 14), 2u);
  EXPECT_EQ(status(open("/alias")), Linked);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("/moved"), 0000}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0604}), 0u);
  auto Unlinked = status(FD);
  EXPECT_EQ(llvm::support::endian::read16le(Unlinked.data() + 14), 0u);
  const auto New = open("/data", 0x202);
  refused(ServiceKind::Fchmod, {New, 0000}, diagnostic::FileModeProvider);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0000}), 0u);
  EXPECT_NE(llvm::support::endian::read64le(status(New).data() + 16),
            llvm::support::endian::read64le(status(FD).data() + 16));
}

TEST_P(DarwinModeTest, MappedObjectKeepsBytesIdentityAndVMLifetime) {
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
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0000}), 0u);
  auto After = Files->mappingSource(FD);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(After));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(After).Lease, Source.Lease);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(After).Bytes.data(),
            Source.Bytes.data());
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0604}), 0u);
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

TEST_P(DarwinModeTest, BothStaticQueryClosuresPrecedeEveryModePreflight) {
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries}) {
    Options = darwin_test::modeOptions();
    Options->ModeMutations.reset();
    Options->Authorization = A;
    reset();
    for (auto K :
         {ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt})
      refused(K, {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX},
              A == DarwinFileAuthorization::StaticOwnerQueries
                  ? diagnostic::FileAuthorizationScope
                  : diagnostic::FileOrdinaryAuthorizationScope);
  }
}
// Independent LP64 stat offsets and declared virtual fixture values. Native
// observations are separate and supply no UID/GID/inode/time constants here.
TEST_P(DarwinModeTest, DirectoryRoutesPreserveTheCompleteIndependentRecord) {
  Options->ModeMutations->Objects["/"] = {71, 72};
  reset();
  const auto FD = open("/");
  Record Expected;
  Expected.fill(0);
  for (unsigned I = 0; I != 8; ++I)
    Expected[I] = Expected[152 + I] = 0xa5;
  llvm::support::endian::write32le(Expected.data() + 8, 7);
  llvm::support::endian::write16le(Expected.data() + 12, 0040700);
  llvm::support::endian::write16le(Expected.data() + 14, 2);
  llvm::support::endian::write64le(Expected.data() + 16, 1);
  llvm::support::endian::write32le(Expected.data() + 24, 501);
  llvm::support::endian::write32le(Expected.data() + 28, 20);
  for (unsigned I = 0; I != 8; ++I)
    llvm::support::endian::write64le(Expected.data() + 40 + I * 8, I + 1);
  llvm::support::endian::write32le(Expected.data() + 120, 512);
  llvm::support::endian::write32le(Expected.data() + 128, 9);
  ASSERT_EQ(status(FD), Expected);
  const auto Allocated = Space->physicalMemory()->allocatedBytes();
  for (auto K :
       {ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt})
    for (uint64_t High : {0ULL, 0xaabbccdd00000000ULL})
      for (uint16_t Mode : {0755, 0750, 0700, 0000}) {
        EXPECT_EQ(ok(K, operands(K, High | FD, High | Mode, "/")), 0u);
        EXPECT_EQ(status(FD), changed(Expected, 0040000 | Mode));
      }
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_EQ(Options->Metadata.at("/").Mode, 0040700);
  EXPECT_EQ(Options->Metadata.at("/").ChangeTime.Seconds, 5);
}

TEST_P(DarwinModeTest, DirectoryGrantsAndNonownerEqualRequestsAreIndependent) {
  const auto FD = open("/");
  const auto Original = status(FD);
  refused(ServiceKind::Fchmod, {FD, 0750}, diagnostic::FileModeProvider);
  EXPECT_EQ(status(FD), Original);
  Options->ModeMutations->Objects["/"] = {71, 72};
  Options->Metadata["/"].UID = 700;
  reset();
  const auto Other = open("/");
  const auto Before = status(Other);
  error(ServiceKind::Fchmod, {Other, 0750}, 1);
  EXPECT_EQ(status(Other), Before);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Other, 0700}), 0u);
  EXPECT_EQ(status(Other), changed(Before, 0040700));
  refused(ServiceKind::Fchmod, {Other, 04700}, diagnostic::FileModePermissions);
}

TEST_P(DarwinModeTest, NamespaceWithoutItsOwnTimeInvalidatesBothModeOrders) {
  for (bool ModeFirst : {false, true}) {
    Options = darwin_test::modeOptions();
    Options->ModeMutations->Objects["/"] = {71, 72};
    Options->MutableDirectories.insert("/");
    reset();
    const auto FD = open("/");
    if (ModeFirst)
      EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0750}), 0u);
    EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/child"), 0700}), 0u);
    refused(ServiceKind::Fstat64, {FD, Base + Page + 8},
            diagnostic::DirectoryMutated);
    refused(ServiceKind::Fchmod, {FD, 0700}, diagnostic::DirectoryMutated);
    EXPECT_EQ(ok(ServiceKind::Rmdir, {path("/child")}), 0u);
    refused(ServiceKind::Fchmod, {FD, 0700}, diagnostic::DirectoryMutated);
    refused(ServiceKind::Fstat64, {FD, Base + Page + 8},
            diagnostic::DirectoryMutated);
  }
}

TEST_P(DarwinModeTest, IndependentNamespaceTimePreservesModeInBothOrders) {
  for (bool ModeFirst : {false, true}) {
    Options = darwin_test::modeOptions();
    Options->ModeMutations->Objects["/"] = {71, 72};
    mutationPolicies();
    const auto FD = open("/");
    auto Expected = status(FD);
    if (ModeFirst) {
      EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0750}), 0u);
      Expected = changed(Expected, 0040750);
    }
    EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/child"), 0700}), 0u);
    Expected = changed(Expected, ModeFirst ? 0040750 : 0040700, 91, 92);
    llvm::support::endian::write16le(Expected.data() + 14, 4);
    llvm::support::endian::write64le(Expected.data() + 104, 64);
    llvm::support::endian::write64le(Expected.data() + 56, 91);
    llvm::support::endian::write64le(Expected.data() + 64, 92);
    EXPECT_EQ(status(FD), Expected);
    if (!ModeFirst) {
      EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0750}), 0u);
      Expected = changed(Expected, 0040750);
      EXPECT_EQ(status(FD), Expected);
    }
    EXPECT_EQ(ok(ServiceKind::Rmdir, {path("/child")}), 0u);
    Expected = changed(Expected, 0040750, 91, 92);
    llvm::support::endian::write16le(Expected.data() + 14, 3);
    llvm::support::endian::write64le(Expected.data() + 104, 48);
    EXPECT_EQ(status(FD), Expected);
    EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0700}), 0u);
    EXPECT_EQ(status(FD), changed(Expected, 0040700));
  }
}

TEST_P(DarwinModeTest, CreatedDirectoriesNeedTheirSeparateModeGrant) {
  for (bool Regular : {false, true})
    for (bool Directory : {false, true}) {
      Options = darwin_test::modeOptions();
      mutationPolicies();
      Options->CreationPolicy->Namespace =
          DarwinNamespaceCreationPolicy{512, 16, 0};
      Options->ModeMutations->CreatedRegularObjects = Regular;
      Options->ModeMutations->CreatedDirectoryObjects = Directory;
      // This creation fixture declares matching observation and explicit actor
      // IDs. Other mode tests retain the mismatching scalar777 authority check.
      reset(501);
      EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/child"), 0700}), 0u);
      const auto FD = open("/child");
      const auto Before = status(FD);
      if (Directory) {
        EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0750}), 0u);
        EXPECT_EQ(status(FD), changed(Before, 0040750, 81, 82));
      } else {
        refused(ServiceKind::Fchmod, {FD, 0750}, diagnostic::FileModeProvider);
        EXPECT_EQ(status(FD), Before);
      }
      // Namespace mutability and the creation flag authorize no initial parent.
      refused(ServiceKind::Fchmod, {open("/"), 0750},
              diagnostic::FileModeProvider);
    }
}

TEST(DarwinModeOptions,
     DirectoryLinksAreNotAliasesAndKnownAliasesStayRejected) {
  auto O = darwin_test::modeOptions();
  O.Directories.insert("/dir");
  O.Metadata["/dir"] = darwin_test::modeMetadata(0040700, 3);
  O.Metadata["/dir"].LinkCount = 7;
  O.ModeMutations->Objects["/dir"] = {71, 72};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/alias");
  O.Metadata["/alias"] = O.Metadata["/dir"];
  auto Error = validateFileOptions(O);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  O.Metadata["/alias"].Device = 8;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.OwnershipMutations.emplace().Objects["/dir"] = {71, 72};
  EXPECT_FALSE(bool(validateFileOptions(O)));
}

TEST_P(DarwinModeTest, DirectoryDescriptionsKeepCursorsAndChildGrantsSeparate) {
  Options->Directories.insert("/dir");
  Options->Metadata["/dir"] = darwin_test::modeMetadata(0040700, 3, 96);
  Options->Files["/dir/leaf"] = {'x'};
  Options->Metadata["/dir/leaf"] = darwin_test::modeMetadata(0100600, 4, 1);
  Options->Directories.insert("/dir/sub");
  Options->Metadata["/dir/sub"] = darwin_test::modeMetadata(0040700, 5);
  Options->DirectoryContents["/dir"] = {{{".", 3, 4, 11, 0},
                                         {"..", 1, 4, 22, 0},
                                         {"leaf", 4, 8, 33, 0},
                                         {"sub", 5, 4, 44, 0}},
                                        1};
  Options->ModeMutations->Objects["/dir"] = {71, 72};
  reset();
  const auto FD = open("/dir"), Independent = open("/dir");
  const auto Copy = ok(ServiceKind::Dup, {FD});
  const auto Before = status(FD);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {FD, Base + Page * 2, 32, Base + 512}),
      32u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Copy, 0750}), 0u);
  EXPECT_EQ(status(Independent), changed(Before, 0040750));
  EXPECT_EQ(ok(ServiceKind::Lseek, {Copy, 0, 1}), 11u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Copy}), 0u);
  EXPECT_EQ(ok(ServiceKind::Chmod, {path("."), 0700}), 0u);
  EXPECT_EQ(ok(ServiceKind::FchmodAt, {Independent, path("."), 0755, 0}), 0u);
  EXPECT_EQ(status(open(".")), changed(Before, 0040755));
  const auto Child = open("leaf");
  const auto ChildBefore = status(Child);
  refused(ServiceKind::Fchmod, {Child, 0640}, diagnostic::FileModeProvider);
  EXPECT_EQ(status(Child), ChildBefore);
  const auto Subdirectory = open("sub");
  const auto SubdirectoryBefore = status(Subdirectory);
  refused(ServiceKind::Fchmod, {Subdirectory, 0750},
          diagnostic::FileModeProvider);
  EXPECT_EQ(status(Subdirectory), SubdirectoryBefore);
  refused(ServiceKind::Fchmod, {open("/"), 0750}, diagnostic::FileModeProvider);
  EXPECT_EQ(ok(ServiceKind::Chdir, {path("/")}), 0u);
  EXPECT_EQ(status(Copy), changed(Before, 0040755));
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Copy, Base + Page * 2, 32, Base + 512}),
      32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page * 2, 8)), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 22u);
}

TEST_P(DarwinModeTest, DirectMoveAndExchangeNeedIndependentNamespaceTime) {
  for (bool Swap : {false, true})
    for (bool NamespaceTime : {false, true})
      for (bool ModeFirst : {false, true}) {
        Options = darwin_test::modeOptions();
        Options->Directories = {"/", "/a", "/b"};
        Options->Metadata["/a"] = darwin_test::modeMetadata(0040700, 3);
        Options->Metadata["/b"] = darwin_test::modeMetadata(0040700, 4);
        Options->MutableDirectories.insert("/");
        Options->DirectoryMutationPolicies["/"] = {16, {91, 92}};
        Options->MovableDirectories.insert("/a");
        Options->SwapRenameDirectories.insert("/");
        Options->ExchangeableDirectories = {"/a", "/b"};
        Options->ModeMutations->Objects["/a"] = {71, 72};
        if (NamespaceTime) {
          Options->DirectoryMutationPolicies["/a"] = {16, {81, 82}};
          Options->DirectoryMutationPolicies["/b"] = {16, {101, 102}};
        }
        reset();
        const auto A = open("/a"), B = open("/b");
        const auto Copy = ok(ServiceKind::Dup, {A});
        const auto Before = status(A), Other = status(B);
        if (ModeFirst)
          EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0750}), 0u);
        const auto From = path("/a"),
                   To = path(Swap ? "/b" : "/moved", Base + 128);
        if (Swap)
          EXPECT_EQ(ok(ServiceKind::RenameAtX, {UINT64_MAX, From, UINT64_MAX,
                                                To, 0x1234567800000002ULL}),
                    0u);
        else
          EXPECT_EQ(ok(ServiceKind::Rename, {From, To}), 0u);
        if (!NamespaceTime) {
          refused(ServiceKind::Fstat64, {A, Base + Page + 8},
                  diagnostic::DirectoryMutated);
          refused(ServiceKind::Fchmod, {Copy, 0700},
                  diagnostic::DirectoryMutated);
          continue;
        }
        auto Expected = changed(Before, ModeFirst ? 0040750 : 0040700, 81, 82);
        EXPECT_EQ(status(Copy), Expected);
        EXPECT_EQ(ok(ServiceKind::Fchmod, {Copy, 0755}), 0u);
        Expected = changed(Expected, 0040755);
        EXPECT_EQ(status(open(Swap ? "/b" : "/moved")), Expected);
        EXPECT_EQ(status(A), Expected);
        EXPECT_EQ(status(B), Swap ? changed(Other, 0040700, 101, 102) : Other);
        // The other object receives no grant merely because its new name used
        // to name the granted object, or because both roots can be exchanged.
        refused(ServiceKind::Fchmod, {B, 0750}, diagnostic::FileModeProvider);
        EXPECT_EQ(Options->ModeMutations->Objects.at("/a").Seconds, 71);
      }
}

TEST_P(DarwinModeTest, HeldRemovedDirectoryAndReusedNameKeepDifferentGrants) {
  Options->Directories.insert("/dir");
  Options->Metadata["/dir"] = darwin_test::modeMetadata(0040700, 3);
  Options->ModeMutations->Objects["/dir"] = {71, 72};
  Options->RemovableDirectories.insert("/dir");
  mutationPolicies();
  Options->DirectoryMutationPolicies["/dir"] = {16, {91, 92}};
  Options->CreationPolicy->Namespace =
      DarwinNamespaceCreationPolicy{512, 16, 0};
  reset(501);
  const auto FD = open("/dir");
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {path("/dir")}), 0u);
  auto Expected = changed(Before, 0040700, 91, 92);
  EXPECT_EQ(status(FD), Expected);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  const auto New = open("/dir");
  const auto NewBefore = status(New);
  refused(ServiceKind::Fchmod, {New, 0750}, diagnostic::FileModeProvider);
  EXPECT_EQ(status(New), NewBefore);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0750}), 0u);
  EXPECT_EQ(status(FD), changed(Expected, 0040750));
  EXPECT_EQ(status(New), NewBefore);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Fchmod, {FD, 0700}, 9);
}

TEST(DarwinModeOptions, CreatedDirectoryGrantRequiresCompleteNamespacePolicy) {
  auto O = darwin_test::modeOptions();
  O.ModeMutations->CreatedDirectoryObjects = true;
  auto Error = validateFileOptions(O);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  DarwinFileCreationPolicy P;
  P.FirstInode = 100;
  P.BlockSize = 512;
  P.Time = {41, 42};
  P.Mutation = {512, {81, 82}};
  O.CreationPolicy = P;
  O.InitialUmask = 022;
  O.MutableDirectories.insert("/");
  Error = validateFileOptions(O);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
  O.CreationPolicy->Namespace = DarwinNamespaceCreationPolicy{512, 16, 0};
  Error = validateFileOptions(O);
  EXPECT_FALSE(bool(Error)) << (Error ? llvm::toString(std::move(Error)) : "");
}

INSTANTIATE_TEST_SUITE_P(Pages, DarwinModeTest,
                         testing::Values(4096ULL, 16384ULL));
} // namespace
} // namespace neverd::emulation::darwin_model
