//===- DarwinCreatedAttributeTests.cpp - Created advisory locks
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original independent mode/ownership provider and object lifetime
/// regressions.
#include "DarwinCreatedAttributeTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinCreatedAttributeTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "memory-only created attributes"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::createdAttributeOptions();
    Credentials = darwin_test::createdAttributeCredentials();
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
        llvm::ArrayRef<uint8_t>((const uint8_t *)Bytes.data(), Bytes.size())));
    return Address;
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result = ProcessResult{
        ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
        ExecutionBackendKind::Unicorn, "memory-only created attributes"};
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
  uint64_t create(llvm::StringRef Name = "/fresh", uint32_t Mode = 0600) {
    return ok(ServiceKind::Open, {path(Name), 0xa02, Mode});
  }

  Record modeChanged(Record B, uint16_t Mode, uint64_t Seconds = 101,
                     uint64_t Nanos = 102) {
    llvm::support::endian::write16le(B.data() + 12, Mode);
    llvm::support::endian::write64le(B.data() + 72, Seconds);
    llvm::support::endian::write64le(B.data() + 80, Nanos);
    return B;
  }
  Record ownerChanged(Record B, uint32_t UID = 501, uint32_t GID = 20,
                      uint64_t Seconds = 101, uint64_t Nanos = 102) {
    llvm::support::endian::write32le(B.data() + 24, UID);
    llvm::support::endian::write32le(B.data() + 28, GID);
    llvm::support::endian::write64le(B.data() + 72, Seconds);
    llvm::support::endian::write64le(B.data() + 80, Nanos);
    return B;
  }
  std::array<uint64_t, 6> modeArgs(ServiceKind K, uint64_t FD, uint64_t Mode,
                                   llvm::StringRef Name = "/fresh") {
    if (K == ServiceKind::Fchmod)
      return {FD, Mode};
    if (K == ServiceKind::FchmodAt)
      return {UINT64_MAX, path(Name), Mode, 0};
    return {path(Name), Mode};
  }
  std::array<uint64_t, 6> ownerArgs(ServiceKind K, uint64_t FD, uint64_t UID,
                                    uint64_t GID,
                                    llvm::StringRef Name = "/fresh") {
    if (K == ServiceKind::Fchown)
      return {FD, UID, GID};
    if (K == ServiceKind::FchownAt)
      return {UINT64_MAX, path(Name), UID, GID, 0};
    return {path(Name), UID, GID};
  }
  static constexpr ServiceKind Modes[] = {
      ServiceKind::Chmod, ServiceKind::Fchmod, ServiceKind::FchmodAt};
  static constexpr ServiceKind Owners[] = {
      ServiceKind::Chown, ServiceKind::Fchown, ServiceKind::FchownAt};
};
TEST(DarwinCreatedAttributeOptions, DefaultsRequireCompleteCreationPolicy) {
  EXPECT_FALSE(DarwinModeMutationOptions{}.CreatedRegularObjects);
  EXPECT_FALSE(DarwinOwnershipMutationOptions{}.CreatedRegularObjects);
  for (bool Owner : {false, true}) {
    auto O = darwin_test::createdAttributeOptions(!Owner, Owner);
    O.CreationPolicy.reset();
    auto E = validateFileOptions(O);
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
    O.ModeMutations->CreatedRegularObjects = false;
    O.OwnershipMutations->CreatedRegularObjects = false;
    EXPECT_FALSE(bool(validateFileOptions(O)));
    for (unsigned I = 0; I != 8; ++I) {
      O = darwin_test::createdAttributeOptions(!Owner, Owner);
      switch (I) {
      case 0:
        O.InitialUmask.reset();
        break;
      case 1:
        O.MutableDirectories.clear();
        break;
      case 2:
        O.Metadata.erase("/");
        break;
      case 3:
        O.CreationPolicy->FirstInode = 2;
        break;
      case 4:
        O.CreationPolicy->BlockSize = 0;
        break;
      case 5:
        O.CreationPolicy->Time.Nanoseconds = 1000000000;
        break;
      case 6:
        O.CreationPolicy->Mutation.Time.Nanoseconds = -1;
        break;
      case 7:
        O.CreationPolicy->Namespace->DirectoryEntrySize = 0;
        break;
      }
      E = validateFileOptions(O);
      EXPECT_TRUE(bool(E)) << Owner << ":" << I;
      llvm::consumeError(std::move(E));
    }
  }
}
TEST(DarwinCreatedAttributeOptions,
     CreationCannotBypassInitialAdmissionOrSharedBudget) {
  for (bool Owner : {false, true}) {
    auto O = darwin_test::createdAttributeOptions();
    auto &Map =
        Owner ? O.OwnershipMutations->Objects : O.ModeMutations->Objects;
    Map["/data"] = {71, 72};
    for (unsigned I = 0; I != 6; ++I) {
      auto Bad = O;
      switch (I) {
      case 0:
        Bad.Metadata["/data"].Flags = 1;
        break;
      case 1:
        Bad.Metadata["/data"].Mode |= 04000;
        break;
      case 2:
        Bad.Metadata["/data"].LinkCount = 2;
        break;
      case 3:
        Bad.Metadata.erase("/data");
        break;
      case 4:
        (Owner ? Bad.OwnershipMutations->Objects
               : Bad.ModeMutations->Objects)["/data"]
            .Nanoseconds = 1000000000;
        break;
      case 5:
        Bad.Files["/alias"] = Bad.Files["/data"];
        Bad.Metadata["/alias"] = Bad.Metadata["/data"];
        break;
      }
      auto E = validateFileOptions(Bad);
      EXPECT_TRUE(bool(E)) << Owner << ":" << I;
      llvm::consumeError(std::move(E));
    }
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  auto O = darwin_test::createdAttributeOptions();
  O.Files.clear();
  O.Metadata.erase("/data");
  O.StandardInput = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.StandardInput->push_back(0);
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DarwinCreatedAttributeOptions, StaticQueriesRejectEvenFalseDeclarations) {
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries})
    for (bool Owner : {false, true}) {
      DarwinFileOptions O;
      O.Authorization = A;
      if (Owner)
        O.OwnershipMutations.emplace();
      else
        O.ModeMutations.emplace();
      auto E = validateFileOptions(O);
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    }
}
TEST_P(DarwinCreatedAttributeTest, IndependentDefaultsAndInitialObjects) {
  for (bool Mode : {false, true})
    for (bool Owner : {false, true}) {
      Options = darwin_test::createdAttributeOptions(Mode, Owner);
      reset();
      const auto Initial = open(), A = create();
      const auto Before = status(A);
      EXPECT_EQ(llvm::support::endian::read32le(Before.data() + 24), 501u);
      refused(ServiceKind::Fchmod, {Initial, 0600},
              diagnostic::FileModeProvider);
      refused(ServiceKind::Fchown, {Initial, UINT32_MAX, UINT32_MAX},
              diagnostic::FileOwnershipProvider);
      if (Mode) {
        EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0640}), 0u);
        EXPECT_EQ(status(A), modeChanged(Before, 0100640));
      } else {
        refused(ServiceKind::Fchmod, {A, 0640}, diagnostic::FileModeProvider);
        EXPECT_EQ(status(A), Before);
      }
      const auto Current = status(A);
      if (Owner) {
        EXPECT_EQ(ok(ServiceKind::Fchown, {A, 501, 31}), 0u);
        EXPECT_EQ(status(A), ownerChanged(Current, 501, 31));
      } else {
        refused(ServiceKind::Fchown, {A, 501, 31},
                diagnostic::FileOwnershipProvider);
        EXPECT_EQ(status(A), Current);
      }
    }
  Options = darwin_test::createdAttributeOptions();
  Options->ModeMutations.reset();
  Options->OwnershipMutations.reset();
  reset();
  const auto A = create();
  const auto Before = status(A);
  refused(ServiceKind::Fchmod, {A, 0600}, diagnostic::FileModeEnvironment);
  refused(ServiceKind::Fchown, {A, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipEnvironment);
  EXPECT_EQ(status(A), Before);
  error(ServiceKind::Fchmod, {UINT64_MAX, 0600}, 9);
  error(ServiceKind::Fchown, {UINT64_MAX, 501, 20}, 9);
}
TEST_P(DarwinCreatedAttributeTest, ExistingCreateAndReplacementKeepOwnGrants) {
  for (bool Created : {false, true}) {
    Options = darwin_test::createdAttributeOptions(Created, Created);
    if (!Created) {
      Options->ModeMutations->Objects["/data"] = {71, 72};
      Options->OwnershipMutations->Objects["/data"] = {81, 82};
    }
    // This independent policy preserves the held initial object's metadata.
    Options->WritableFiles.insert("/data");
    Options->MutationPolicies["/data"] = {512, {101, 102}};
    reset();
    const auto Old = open(),
               Existing = ok(ServiceKind::Open, {path("/data"), 0x200, 0600});
    const auto Before = status(Old);
    if (Created) {
      refused(ServiceKind::Fchmod, {Existing, 0640},
              diagnostic::FileModeProvider);
      refused(ServiceKind::Fchown, {Existing, 501, 20},
              diagnostic::FileOwnershipProvider);
      EXPECT_EQ(status(Old), Before);
    } else {
      EXPECT_EQ(ok(ServiceKind::Fchmod, {Existing, 0640}), 0u);
      EXPECT_EQ(status(Old), modeChanged(Before, 0100640, 71, 72));
      EXPECT_EQ(ok(ServiceKind::Fchown, {Existing, 501, 20}), 0u);
      EXPECT_EQ(status(Old),
                ownerChanged(modeChanged(Before, 0100640), 501, 20, 81, 82));
    }
    error(ServiceKind::Open, {path("/data"), 0xa02, 0600}, 17);
    EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
    const auto New = create("/data");
    const auto Held = status(Old);
    EXPECT_NE(llvm::support::endian::read64le(status(New).data() + 16),
              llvm::support::endian::read64le(Held.data() + 16));
    if (Created) {
      EXPECT_EQ(ok(ServiceKind::Fchmod, {New, 0640}), 0u);
      EXPECT_EQ(ok(ServiceKind::Fchown, {New, 501, 31}), 0u);
      refused(ServiceKind::Fchmod, {Old, 0600}, diagnostic::FileModeProvider);
      refused(ServiceKind::Fchown, {Old, UINT32_MAX, UINT32_MAX},
              diagnostic::FileOwnershipProvider);
    } else {
      refused(ServiceKind::Fchmod, {New, 0600}, diagnostic::FileModeProvider);
      refused(ServiceKind::Fchown, {New, UINT32_MAX, UINT32_MAX},
              diagnostic::FileOwnershipProvider);
      EXPECT_EQ(ok(ServiceKind::Fchmod, {Old, 0604}), 0u);
      EXPECT_EQ(status(Old), modeChanged(Held, 0100604, 71, 72));
      EXPECT_EQ(ok(ServiceKind::Fchown, {Old, 501, 31}), 0u);
      EXPECT_EQ(status(Old),
                ownerChanged(modeChanged(Held, 0100604), 501, 31, 81, 82));
    }
  }
}
TEST_P(DarwinCreatedAttributeTest,
       ModeAndOwnershipUseIndependentExplicitTimes) {
  Options->ModeMutations->Objects["/data"] = {71, 72};
  Options->OwnershipMutations->Objects["/data"] = {81, 82};
  reset();
  const auto Old = open(), A = create();
  const auto Before = status(A);
  const auto Initial = status(Old);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0600}), 0u);
  EXPECT_EQ(status(A), modeChanged(Before, 0100600));
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, 501, 20}), 0u);
  EXPECT_EQ(status(A), ownerChanged(Before));
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Old, 0640}), 0u);
  EXPECT_EQ(status(Old), modeChanged(Initial, 0100640, 71, 72));
  EXPECT_EQ(ok(ServiceKind::Fchown, {Old, 501, 31}), 0u);
  EXPECT_EQ(status(Old),
            ownerChanged(modeChanged(Initial, 0100640), 501, 31, 81, 82));
  EXPECT_EQ(Options->CreationPolicy->Time.Seconds, 41);
  EXPECT_EQ(Options->Metadata.at("/data").Mode, 0100600);
}
TEST_P(DarwinCreatedAttributeTest,
       CarriersAndDirtyStateKeepCurrentObjectContract) {
  const auto A = create(), B = open("/fresh"), D = ok(ServiceKind::Dup, {A});
  const std::array<uint8_t, 4> Input{'X', 0, 255, 'Y'};
  llvm::cantFail(Space->write(Base + Page * 2, Input));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 4}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 3, 0}), 3u);
  auto Current = status(A);
  for (auto K : Modes)
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      EXPECT_EQ(ok(K, modeArgs(K, High | B, High | 0x7fff0000 | 0640)), 0u);
      Current = modeChanged(Current, 0100640);
      for (auto FD : {A, B, D})
        EXPECT_EQ(status(FD), Current);
    }
  for (auto K : Owners)
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      EXPECT_EQ(
          ok(K, ownerArgs(K, High | B, High | UINT32_MAX, High | UINT32_MAX)),
          0u);
      EXPECT_EQ(status(A), Current);
      EXPECT_EQ(ok(K, ownerArgs(K, High | B, High | 501, High | 31)), 0u);
      Current = ownerChanged(Current, 501, 31);
      for (auto FD : {A, B, D})
        EXPECT_EQ(status(FD), Current);
    }
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {B, Base + Page * 2 + 16, 4}), 4u);
  std::array<uint8_t, 4> Bytes;
  llvm::cantFail(Space->read(Base + Page * 2 + 16, Bytes));
  EXPECT_EQ(Bytes, Input);
}
TEST_P(DarwinCreatedAttributeTest,
       InactiveOwnershipStillNeedsProviderAndPreservesEveryByte) {
  for (bool Owner : {false, true}) {
    Options = darwin_test::createdAttributeOptions(true, Owner);
    reset();
    const auto A = create();
    const auto Before = status(A);
    for (auto K : Owners) {
      if (Owner)
        EXPECT_EQ(ok(K, ownerArgs(K, A, UINT32_MAX, UINT32_MAX)), 0u);
      else
        refused(K, ownerArgs(K, A, UINT32_MAX, UINT32_MAX),
                diagnostic::FileOwnershipProvider);
      EXPECT_EQ(status(A), Before);
    }
    if (Owner) {
      EXPECT_EQ(ok(ServiceKind::Fchown, {A, 501, UINT32_MAX}), 0u);
      const auto Current = ownerChanged(Before);
      EXPECT_EQ(status(A), Current);
      EXPECT_EQ(ok(ServiceKind::Fchown, {A, UINT32_MAX, UINT32_MAX}), 0u);
      EXPECT_EQ(status(A), Current);
    }
  }
}
TEST_P(DarwinCreatedAttributeTest,
       ReadOnlyDuplicateAndIndependentDescriptorsShareObjectGrant) {
  const auto A = create(), B = open("/fresh"), D = ok(ServiceKind::Dup, {A}),
             F = ok(ServiceKind::Fcntl, {A, 0, 10});
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, 20}), 20u);
  auto Current = status(A);
  for (auto FD : {B, D, F, uint64_t(20)}) {
    EXPECT_EQ(ok(ServiceKind::Fchmod, {FD, 0640}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fchown, {FD, 501, 31}), 0u);
    Current = ownerChanged(modeChanged(Current, 0100640), 501, 31);
    EXPECT_EQ(status(A), Current);
    EXPECT_EQ(status(B), Current);
  }
  for (auto FD : {A, D, F, uint64_t(20)}) {
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    error(ServiceKind::Fchmod, {FD, 0600}, 9);
    error(ServiceKind::Fchown, {FD, 501, 20}, 9);
    EXPECT_EQ(status(B), Current);
  }
  EXPECT_EQ(ok(ServiceKind::Fchmod, {B, 0604}), 0u);
  EXPECT_EQ(status(B), modeChanged(Current, 0100604));
}
TEST_P(DarwinCreatedAttributeTest, RefusalsBeforeProviderKeepEveryObservation) {
  const auto A = create();
  const auto Before = status(A);
  for (unsigned Repeat = 0; Repeat != 2; ++Repeat) {
    error(ServiceKind::Chmod, {UINT64_MAX, 07000}, 14);
    error(ServiceKind::Fchmod, {UINT64_MAX, 07000}, 9);
    error(ServiceKind::FchmodAt, {UINT64_MAX, UINT64_MAX, 07000, 8}, 22);
    error(ServiceKind::FchmodAt, {UINT64_MAX, path("data"), 0600, 0}, 9);
    error(ServiceKind::Chown, {UINT64_MAX, 501, 20}, 14);
    error(ServiceKind::Fchown, {UINT64_MAX, 501, 20}, 9);
    error(ServiceKind::FchownAt, {UINT64_MAX, UINT64_MAX, 501, 20, 0x800}, 22);
    error(ServiceKind::FchownAt, {UINT64_MAX, path("data"), 501, 20, 0}, 9);
    refused(ServiceKind::Fchmod, {A, 07000}, diagnostic::FileModePermissions);
    error(ServiceKind::Fchown, {A, 700, UINT32_MAX}, 1);
    refused(ServiceKind::Fchown, {A, UINT32_MAX, 777},
            diagnostic::FileOrdinaryAuthorizationGroups);
    EXPECT_EQ(status(A), Before);
  }
  // Ordinary security-attribute refusal is separate from namespace/create
  // admission, which correctly refuses flagged mutable initial objects.
  Options = darwin_test::createdAttributeOptions(false, false);
  Options->CreationPolicy.reset();
  Options->MutableDirectories.clear();
  Options->DirectoryMutationPolicies.clear();
  Options->Metadata["/data"].Flags = 1;
  reset();
  const auto Old = open();
  const auto Current = status(Old);
  refused(ServiceKind::Fchmod, {Old, 0600},
          diagnostic::FileModeSecurityAttributes);
  refused(ServiceKind::Fchown, {Old, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOwnershipSecurityAttributes);
  EXPECT_EQ(status(Old), Current);
  Options->Metadata["/data"].Flags = 0;
  Credentials.reset();
  reset();
  const auto NoCredentials = open();
  const auto Unknown = status(NoCredentials);
  refused(ServiceKind::Fchmod, {NoCredentials, 0600},
          diagnostic::FileOrdinaryAuthorizationCredentials);
  refused(ServiceKind::Fchown, {NoCredentials, UINT32_MAX, UINT32_MAX},
          diagnostic::FileOrdinaryAuthorizationCredentials);
  EXPECT_EQ(status(NoCredentials), Unknown);
}
TEST_P(DarwinCreatedAttributeTest, LinkRenameAndZeroNamesKeepObjectGrant) {
  const auto A = create();
  EXPECT_EQ(ok(ServiceKind::Link, {path("/fresh"), path("/alias", Base + 128)}),
            0u);
  const auto B = open("/alias");
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/fresh")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Before = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Before.data() + 14), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {B, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {B, 501, 31}), 0u);
  const auto Held = ownerChanged(modeChanged(Before, 0100640), 501, 31);
  EXPECT_EQ(status(A), Held);
  const auto Fresh = create();
  EXPECT_NE(llvm::support::endian::read64le(status(Fresh).data() + 16),
            llvm::support::endian::read64le(Held.data() + 16));
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Fresh, 0604}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {Fresh, 501, 20}), 0u);
  EXPECT_EQ(status(A), Held);
}
TEST_P(DarwinCreatedAttributeTest, MappingLeaseKeepsAttributeAuthority) {
  const auto A = create(), B = open("/fresh");
  const std::array<uint8_t, 3> Input{'X', 0, 255};
  llvm::cantFail(Space->write(Base + Page * 2, Input));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 3}), 3u);
  auto Snapshot = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Snapshot));
  ProcessOptions V;
  V.MemoryLimit = Page * 16;
  V.StackSize = Page;
  MemoryLayout Layout{Page, Base + Page * 8, {}};
  DarwinMemory VM(*Space, Layout, V);
  auto Mapped = VM.handle(ServiceKind::Mmap, {0, 0, {0, Page, 1, 2, A, 0}, {}},
                          *Files, Result);
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  ASSERT_TRUE(*Mapped) << Result.Diagnostic;
  ASSERT_FALSE((**Mapped).Error);
  const auto Address = (**Mapped).Value;
  const auto Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/fresh")}), 0u);
  const auto Before = status(B);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Fchmod, {A, 0600}, 9);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {B, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {B, 501, 31}), 0u);
  EXPECT_EQ(status(B), ownerChanged(modeChanged(Before, 0100640), 501, 31));
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  error(ServiceKind::Fchown, {B, UINT32_MAX, UINT32_MAX}, 9);
  std::array<uint8_t, 3> Bytes;
  ASSERT_FALSE(bool(Space->read(Address, Bytes)));
  EXPECT_EQ(Bytes, Input);
  EXPECT_GT(std::get<DarwinFiles::Mapping>(Snapshot).Lease.use_count(), 1);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  auto Unmapped = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}},
                            *Files, Result);
  ASSERT_TRUE(bool(Unmapped));
  ASSERT_TRUE(*Unmapped);
  EXPECT_FALSE((**Unmapped).Error);
}
TEST_P(DarwinCreatedAttributeTest, FailedCreationPreservesBudgetsAndNextInode) {
  Options->DescriptorLimit = 4;
  reset();
  const auto Old = open();
  const auto Before = status(Old);
  for (unsigned I = 0; I != 2; ++I)
    error(ServiceKind::Open, {path("/fresh"), 0xa02, 0600}, 24);
  EXPECT_EQ(status(Old), Before);
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  const auto A = create();
  EXPECT_EQ(A, Old);
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 16), 100u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, 501, 31}), 0u);
  Options = darwin_test::createdAttributeOptions();
  Options->CreationPolicy->FirstInode = UINT64_MAX;
  reset();
  const auto Last = create();
  const auto Current = status(Last);
  for (unsigned I = 0; I != 2; ++I)
    refused(ServiceKind::Open, {path("/next"), 0xa02, 0600},
            diagnostic::FileCreationInode);
  EXPECT_EQ(status(Last), Current);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Last, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {Last, 501, 31}), 0u);
  for (bool Entries : {false, true}) {
    Options = darwin_test::createdAttributeOptions();
    Options->Files.clear();
    Options->Metadata.erase("/data");
    if (Entries)
      for (unsigned I = 0; I != darwin_file_limits::Files - 1; ++I)
        Options->Files["/f" + std::to_string(I)] = {};
    else
      Options->StandardInput =
          std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
    reset();
    const auto Dir = open("/", 0x100000);
    const auto Initial = status(Dir);
    for (unsigned I = 0; I != 2; ++I)
      refused(ServiceKind::Open, {path("/fresh"), 0xa02, 0600},
              diagnostic::FileCreationLimit);
    EXPECT_EQ(status(Dir), Initial);
    error(ServiceKind::Fchmod, {4, 0600}, 9);
    error(ServiceKind::Fchown, {4, 501, 20}, 9);
  }
}
TEST_P(DarwinCreatedAttributeTest,
       DirectoriesAndLinksNeverAcquireRegularGrant) {
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::Symlink, {path("target"), path("/link", Base + 128)}),
      0u);
  const auto D = open("/dir", 0x100000), L = open("/link", 0x200000);
  const auto DirectoryBefore = status(D);
  for (auto FD : {D, L, uint64_t(0), uint64_t(1), uint64_t(2)}) {
    refused(ServiceKind::Fchmod, {FD, 0600},
            FD == D ? diagnostic::FileModeProvider
                    : diagnostic::FileModeTarget);
    refused(ServiceKind::Fchown, {FD, UINT32_MAX, UINT32_MAX},
            FD == D || FD == L ? diagnostic::FileOwnershipProvider
                               : diagnostic::FileOwnershipTarget);
  }
  EXPECT_EQ(status(D), DirectoryBefore);
  refused(ServiceKind::FchmodAt, {UINT64_MAX, path("/link"), 0600, 0x20},
          diagnostic::FileModeTarget);
  refused(ServiceKind::FchownAt,
          {UINT64_MAX, path("/link"), UINT32_MAX, UINT32_MAX, 0x20},
          diagnostic::FileOwnershipProvider);
  const auto A = create();
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, 501, 31}), 0u);
}
TEST_P(DarwinCreatedAttributeTest, OtherAuthoritiesRemainIndependent) {
  Options->ModeMutations->Objects["/data"] = {71, 72};
  Options->OwnershipMutations->Objects["/data"] = {81, 82};
  reset();
  const auto Old = open(), A = create();
  refused(ServiceKind::Fsync, {A}, diagnostic::FileSynchronizationEnvironment);
  refused(ServiceKind::Fdatasync, {A},
          diagnostic::FileDataSynchronizationEnvironment);
  refused(ServiceKind::Flock, {A, 6}, diagnostic::FileAdvisoryLockEnvironment);
  EXPECT_FALSE(invoke(ServiceKind::FgetXattr,
                      {A, path("tag", Base + 128), Base + Page * 2, 4, 0, 0}));
  const auto Reason = Result.Diagnostic;
  const auto Current = status(A);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, 501, 31}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::FgetXattr,
                      {A, path("tag", Base + 128), Base + Page * 2, 4, 0, 0}));
  EXPECT_EQ(Result.Diagnostic, Reason);
  EXPECT_EQ(status(A), ownerChanged(modeChanged(Current, 0100640), 501, 31));
  error(ServiceKind::Write, {Old, Base + Page * 2, 1}, 9);
  refused(ServiceKind::Open, {path("/data"), 2}, diagnostic::FileNotWritable);
  const auto Initial = status(Old);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Old, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {Old, 501, 20}), 0u);
  EXPECT_EQ(status(Old),
            ownerChanged(modeChanged(Initial, 0100640), 501, 20, 81, 82));
  error(ServiceKind::Write, {Old, Base + Page * 2, 1}, 9);
  refused(ServiceKind::Open, {path("/data"), 2}, diagnostic::FileNotWritable);
}
TEST_P(DarwinCreatedAttributeTest, InvalidatedMetadataRemainsUnknown) {
  Options->ModeMutations->Objects["/data"] = {71, 72};
  Options->OwnershipMutations->Objects["/data"] = {81, 82};
  reset();
  const auto Old = open();
  EXPECT_EQ(ok(ServiceKind::Fchmod, {Old, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  const auto New = create("/data");
  const auto Before = status(New);
  Record Guard;
  Guard.fill(0xa5);
  llvm::cantFail(Space->write(Base + Page, Guard));
  refused(ServiceKind::Fstat64, {Old, Base + Page + 8},
          diagnostic::FileMutatedMetadata);
  Record After;
  llvm::cantFail(Space->read(Base + Page, After));
  EXPECT_EQ(After, Guard);
  refused(ServiceKind::Fchmod, {Old, 0600}, diagnostic::FileMutatedMetadata);
  refused(ServiceKind::Fchown, {Old, UINT32_MAX, UINT32_MAX},
          diagnostic::FileMutatedMetadata);
  EXPECT_EQ(status(New), Before);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {New, 0604}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {New, 501, 31}), 0u);
  EXPECT_EQ(status(New), ownerChanged(modeChanged(Before, 0100604), 501, 31));
}
INSTANTIATE_TEST_SUITE_P(Pages, DarwinCreatedAttributeTest,
                         testing::Values(uint64_t(4096), uint64_t(16384)));
} // namespace
} // namespace neverd::emulation::darwin_model
