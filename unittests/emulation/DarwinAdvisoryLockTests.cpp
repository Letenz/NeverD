//===- DarwinAdvisoryLockTests.cpp - Description-owned ordinary lock contracts
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original advisory lock object and open-description lifetime regressions.
#include "DarwinAdvisoryLockTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinAdvisoryLockTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "memory-only advisory locks"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::advisoryLockOptions();
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
    Result = ProcessResult{
        ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
        ExecutionBackendKind::Unicorn, "memory-only advisory locks"};
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
TEST(DarwinAdvisoryLockOptions, CompleteOrdinaryObjectsAndQueryClosure) {
  const auto Original = darwin_test::advisoryLockOptions();
  EXPECT_FALSE(bool(validateFileOptions(Original)));
  for (unsigned I = 0; I != 10; ++I) {
    auto O = Original;
    switch (I) {
    case 0:
      O.AdvisoryLocks->Objects.insert("/missing");
      break;
    case 1:
      O.AdvisoryLocks->Objects.insert("/");
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
      O.Files["/alias"] = O.Files["/data"];
      O.Metadata["/alias"] = O.Metadata["/data"];
      break;
    case 7:
      O.AdvisoryLocks->Objects.insert("relative");
      break;
    case 8:
      O.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
      O.AdvisoryLocks->Objects.insert("/link");
      break;
    case 9:
      O.AdvisoryLocks->Objects.insert("/data/../data");
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
    O.AdvisoryLocks->Objects.clear();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST(DarwinAdvisoryLockOptions, ReferencesShareTheInputByteBudget) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 6);
  O.Metadata["/a"] =
      darwin_test::modeMetadata(0100000, 1, O.Files["/a"].size());
  O.AdvisoryLocks.emplace().Objects.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files["/a"].resize(darwin_file_limits::Bytes - 12);
  O.Metadata["/a"].Size = O.Files["/a"].size();
  O.ModeMutations.emplace().Objects["/a"] = {1, 2};
  O.OwnershipMutations.emplace().Objects["/a"] = {3, 4};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_P(DarwinAdvisoryLockTest, ReadonlyCarriersReentryAndWholeRecords) {
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    reset();
    const auto A = open(), B = open(), D = ok(ServiceKind::Dup, {A});
    const auto Before = status(A);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 2, 0}), 2u);
    EXPECT_EQ(ok(ServiceKind::Flock, {High | A, High | 2, UINT64_MAX}), 0u);
    EXPECT_EQ(ok(ServiceKind::Flock, {High | D, High | 6}), 0u);
    error(ServiceKind::Flock, {High | B, High | 5}, 35);
    for (auto FD : {A, B, D})
      EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Flock, {High | D, High | 8}), 0u);
    EXPECT_EQ(ok(ServiceKind::Flock, {High | B, High | 6}), 0u);
  }
}
TEST_P(DarwinAdvisoryLockTest,
       FailedUpgradeRetainsSharedLockAndDowngradeReplaces) {
  const auto A = open(), B = open(), C = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 5}), 0u);
  error(ServiceKind::Flock, {A, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 8}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  error(ServiceKind::Flock, {C, 5}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {C, 5}), 0u);
  error(ServiceKind::Flock, {A, 6}, 35);
}
TEST_P(DarwinAdvisoryLockTest, ActionPrecedenceIgnoresUnrelatedBits) {
  const auto A = open(), B = open();
  const auto Before = status(A);
  for (uint32_t How : {0u, 4u, 0x40000000u})
    error(ServiceKind::Flock, {A, How}, 9);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 0x40000007}), 0u);
  error(ServiceKind::Flock, {B, 5}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 0x4000000f}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 0x4000000a}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 5}), 0u);
  EXPECT_EQ(status(A), Before);
}
TEST_P(DarwinAdvisoryLockTest, DescriptorAndVnodeActionPrecedeProvider) {
  error(ServiceKind::Flock, {UINT64_MAX, 0}, 9);
  for (uint64_t FD : {0u, 1u, 2u})
    refused(ServiceKind::Flock, {FD, 0}, diagnostic::FileAdvisoryLockKind);
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  Options->Metadata["/link"] = darwin_test::modeMetadata(0120777, 3, 4);
  reset();
  const auto Directory = open("/"), Link = open("/link", 0x200000);
  for (auto FD : {Directory, Link}) {
    error(ServiceKind::Flock, {FD, 0}, 9);
    refused(ServiceKind::Flock, {FD, 6}, diagnostic::FileAdvisoryLockTarget);
  }
}
TEST_P(DarwinAdvisoryLockTest,
       MissingAndEmptyProvidersStayDistinctEvenForUnlock) {
  for (bool Empty : {false, true}) {
    Options = darwin_test::advisoryLockOptions();
    if (Empty)
      Options->AdvisoryLocks->Objects.clear();
    else
      Options->AdvisoryLocks.reset();
    reset();
    const auto A = open();
    const auto Before = status(A);
    error(ServiceKind::Flock, {A, 0}, 9);
    for (uint32_t How : {1u, 2u, 8u})
      refused(ServiceKind::Flock, {A, How},
              Empty ? diagnostic::FileAdvisoryLockProvider
                    : diagnostic::FileAdvisoryLockEnvironment);
    EXPECT_EQ(status(A), Before);
  }
}
TEST_P(DarwinAdvisoryLockTest,
       CredentialsAndModeDoNotInventAdditionalAuthorization) {
  for (unsigned Subject = 0; Subject != 3; ++Subject) {
    Options->Metadata["/data"].Mode = 0100000;
    if (Subject == 0)
      Credentials.reset();
    else {
      Credentials = darwin_test::modeCredentials();
      Credentials->EffectiveUID = Subject == 1 ? 900 : 0;
    }
    reset();
    const auto A = open(), B = open();
    EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
    error(ServiceKind::Flock, {B, 6}, 35);
    EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
    EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  }
}
TEST_P(DarwinAdvisoryLockTest, DupFcntlAndFinalCloseOwnOneLock) {
  const auto A = open(), B = open(), D = ok(ServiceKind::Dup, {A}),
             F = ok(ServiceKind::Fcntl, {A, 0, 10});
  EXPECT_EQ(ok(ServiceKind::Flock, {F, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {D, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Close, {F}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
}
TEST_P(DarwinAdvisoryLockTest,
       Dup2SameOwnerAndReplacingFinalTargetReleaseCorrectly) {
  const auto A = open(), B = open(), D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, D}), D);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, A}), A);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {B, D}), D);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {D, 8}), 0u);
}
TEST_P(DarwinAdvisoryLockTest,
       FailedDupAndReusedDescriptorPreserveOtherOwners) {
  Options->DescriptorLimit = 5;
  reset();
  const auto A = open(), B = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  error(ServiceKind::Dup, {A}, 24);
  error(ServiceKind::Dup2, {UINT64_MAX, A}, 9);
  error(ServiceKind::Fcntl, {A, 0, 5}, 22);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  const auto New = open();
  EXPECT_EQ(New, A);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  error(ServiceKind::Flock, {New, 5}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {New, 8}), 0u);
  error(ServiceKind::Flock, {New, 5}, 35);
}
TEST_P(DarwinAdvisoryLockTest,
       IndependentDescriptionsAndObjectsKeepSeparateLocks) {
  Options->Files["/other"] = Options->Files["/data"];
  Options->Metadata["/other"] = darwin_test::modeMetadata(0100600, 3, 6);
  Options->AdvisoryLocks->Objects.insert("/other");
  reset();
  const auto A = open(), B = open(), Other = open("/other");
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {Other, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 8}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {Other, 6}), 0u);
}
TEST_P(DarwinAdvisoryLockTest,
       HardlinksRenameAndHeldZeroNameUnlinkKeepObjectGrant) {
  mutationPolicies();
  const auto A = open();
  EXPECT_EQ(ok(ServiceKind::Link, {path("/data"), path("/alias", Base + 128)}),
            0u);
  const auto Alias = open("/alias");
  EXPECT_EQ(ok(ServiceKind::Flock, {Alias, 2}), 0u);
  error(ServiceKind::Flock, {A, 6}, 35);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Before = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Before.data() + 14), 0u);
  error(ServiceKind::Flock, {A, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {Alias, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  EXPECT_EQ(status(A), Before);
}
TEST_P(DarwinAdvisoryLockTest, CreatedAndReusedNamesCannotBorrowInitialGrant) {
  mutationPolicies();
  const auto A = open(), B = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  const auto New = open("/data", 0x202);
  const auto Created = status(New);
  refused(ServiceKind::Flock, {New, 8}, diagnostic::FileAdvisoryLockProvider);
  refused(ServiceKind::Flock, {New, 6}, diagnostic::FileAdvisoryLockProvider);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(status(New), Created);
}
TEST_P(DarwinAdvisoryLockTest,
       MappingAndRetainedContentsDoNotKeepClosedDescriptionLock) {
  const auto A = open(), B = open(), D = ok(ServiceKind::Dup, {A});
  auto Snapshot = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Snapshot));
  ProcessOptions VMOptions;
  VMOptions.MemoryLimit = Page * 16;
  VMOptions.StackSize = Page;
  MemoryLayout Layout{Page, Base + Page * 8, {}};
  DarwinMemory VM(*Space, Layout, VMOptions);
  auto Mapped = VM.handle(ServiceKind::Mmap, {0, 0, {0, Page, 1, 2, A, 0}, {}},
                          *Files, Result);
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  ASSERT_TRUE(*Mapped) << Result.Diagnostic;
  ASSERT_FALSE((**Mapped).Error);
  const auto Address = (**Mapped).Value,
             Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Address, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  EXPECT_GT(std::get<DarwinFiles::Mapping>(Snapshot).Lease.use_count(), 1);
  auto Unmapped = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}},
                            *Files, Result);
  ASSERT_TRUE(bool(Unmapped)) << llvm::toString(Unmapped.takeError());
  ASSERT_TRUE(*Unmapped);
  EXPECT_FALSE((**Unmapped).Error);
}
TEST_P(DarwinAdvisoryLockTest,
       XattrInvalidationDoesNotEraseGrantOrRestoreMetadata) {
  Options->ExtendedAttributes["/data"] = {{"user.lock", {5}}};
  Options->MutableExtendedAttributes.insert("/data");
  reset();
  const auto A = open(), B = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  llvm::cantFail(Space->writeInteger(Base + Page * 2, 6, 1));
  EXPECT_EQ(ok(ServiceKind::FsetXattr,
               {A, path("user.lock", Base + 128), Base + Page * 2, 1}),
            0u);
  refused(ServiceKind::Fstat64, {A, Base + Page},
          diagnostic::FileMutatedMetadata);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  refused(ServiceKind::Fstat64, {A, Base + Page},
          diagnostic::FileMutatedMetadata);
}
TEST_P(DarwinAdvisoryLockTest, ModeOwnershipAndLocksKeepIndependentPolicies) {
  Options->ModeMutations.emplace().Objects["/data"] = {71, 72};
  Options->OwnershipMutations.emplace().Objects["/data"] = {81, 82};
  Credentials->GroupAccessList = {20, 31};
  reset();
  const auto A = open(), B = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0000}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, UINT32_MAX, 31}), 0u);
  const auto Before = status(A);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(llvm::support::endian::read16le(Before.data() + 12), 0100000);
  EXPECT_EQ(llvm::support::endian::read32le(Before.data() + 28), 31u);
}
TEST_P(DarwinAdvisoryLockTest, DataAndCursorChangesDoNotEraseLocks) {
  mutationPolicies();
  const auto A = open("/data", 2), B = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 2}), 0u);
  llvm::cantFail(Space->writeInteger(Base + Page * 2, 'x', 1));
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + Page * 2, 1, 0}), 1u);
  const auto Before = status(A);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 3, 0}), 3u);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Read, {B, Base + Page * 2, 6}), 6u);
  std::array<uint8_t, 6> Bytes;
  llvm::cantFail(Space->read(Base + Page * 2, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'x', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinAdvisoryLockTest, BlockingConflictRefusesBeforeDroppingOldLock) {
  const auto A = open(), B = open();
  const auto Before = status(A);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 1}), 0u);
  refused(ServiceKind::Flock, {A, 2}, diagnostic::FileAdvisoryLockWait);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 8}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 2}), 0u);
  refused(ServiceKind::Flock, {A, 1}, diagnostic::FileAdvisoryLockWait);
  error(ServiceKind::Flock, {A, 5}, 35);
  EXPECT_EQ(status(A), Before);
}
TEST_P(DarwinAdvisoryLockTest, QueryClosurePrecedesDescriptorAndAction) {
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries}) {
    Options = darwin_test::advisoryLockOptions();
    Options->AdvisoryLocks.reset();
    Options->Authorization = A;
    reset();
    refused(ServiceKind::Flock, {UINT64_MAX, 0, UINT64_MAX},
            A == DarwinFileAuthorization::StaticOwnerQueries
                ? diagnostic::FileAuthorizationScope
                : diagnostic::FileOrdinaryAuthorizationScope);
  }
}
INSTANTIATE_TEST_SUITE_P(Pages, DarwinAdvisoryLockTest,
                         testing::Values(4096ULL, 16384ULL));
} // namespace
} // namespace neverd::emulation::darwin_model
