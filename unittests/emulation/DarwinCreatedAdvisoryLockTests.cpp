//===- DarwinCreatedAdvisoryLockTests.cpp - Created advisory locks
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original created advisory provider and object lifetime regressions.
#include "DarwinCreatedAdvisoryLockTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinCreatedAdvisoryLockTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "memory-only created advisory locks"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::createdAdvisoryLockOptions();
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
        ExecutionBackendKind::Unicorn, "memory-only created advisory locks"};
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
};
TEST(DarwinCreatedAdvisoryLockOptions, DefaultsRequireCompleteCreationPolicy) {
  EXPECT_FALSE(DarwinAdvisoryLockOptions{}.CreatedRegularObjects);
  auto O = darwin_test::createdAdvisoryLockOptions();
  O.CreationPolicy.reset();
  auto E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
  O.AdvisoryLocks->CreatedRegularObjects = false;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  for (unsigned I = 0; I != 8; ++I) {
    O = darwin_test::createdAdvisoryLockOptions();
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
      O.CreationPolicy->Mutation.AllocationUnit = 0;
      break;
    case 7:
      O.CreationPolicy->Namespace->DirectoryEntrySize = 0;
      break;
    }
    E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << I;
    llvm::consumeError(std::move(E));
  }
}
TEST(DarwinCreatedAdvisoryLockOptions,
     CreationCannotBypassInitialAdmissionOrSharedBudget) {
  auto O = darwin_test::createdAdvisoryLockOptions();
  O.AdvisoryLocks->Objects.insert("/data");
  O.Metadata["/data"].Flags = 1;
  auto E = validateFileOptions(O);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileAdvisoryLockOption);
  O.Metadata["/data"].Flags = 0;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.AdvisoryLocks->Objects.clear();
  O.Files.clear();
  O.Metadata.erase("/data");
  O.StandardInput = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.StandardInput->push_back(0);
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DarwinCreatedAdvisoryLockOptions,
     StaticQueriesRejectEvenFalseDeclarations) {
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries}) {
    DarwinFileOptions O;
    O.Authorization = A;
    O.AdvisoryLocks.emplace();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST_P(DarwinCreatedAdvisoryLockTest, IndependentDefaultsAndInitialObjects) {
  const auto Initial = open(), Created = create();
  refused(ServiceKind::Flock, {Initial, 6},
          diagnostic::FileAdvisoryLockProvider);
  EXPECT_EQ(ok(ServiceKind::Flock, {Created, 6}), 0u);
  Options = darwin_test::createdAdvisoryLockOptions(false);
  Options->AdvisoryLocks->Objects.insert("/data");
  reset();
  const auto Old = open(), New = create();
  EXPECT_EQ(ok(ServiceKind::Flock, {Old, 6}), 0u);
  refused(ServiceKind::Flock, {New, 6}, diagnostic::FileAdvisoryLockProvider);
  Options->AdvisoryLocks.reset();
  reset();
  const auto Unknown = create();
  refused(ServiceKind::Flock, {Unknown, 8},
          diagnostic::FileAdvisoryLockEnvironment);
  error(ServiceKind::Flock, {UINT64_MAX, 6}, 9);
}
TEST_P(DarwinCreatedAdvisoryLockTest,
       ExistingCreateAndReplacementKeepOwnGrants) {
  const auto Old = open(),
             Existing = ok(ServiceKind::Open, {path("/data"), 0x200, 0600});
  refused(ServiceKind::Flock, {Existing, 6},
          diagnostic::FileAdvisoryLockProvider);
  error(ServiceKind::Open, {path("/data"), 0xa02, 0600}, 17);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  const auto Replacement = create("/data");
  EXPECT_EQ(ok(ServiceKind::Flock, {Replacement, 6}), 0u);
  refused(ServiceKind::Flock, {Old, 6}, diagnostic::FileAdvisoryLockProvider);
  Options = darwin_test::createdAdvisoryLockOptions(false);
  Options->AdvisoryLocks->Objects.insert("/data");
  reset();
  const auto Held = open();
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  const auto Fresh = create("/data");
  refused(ServiceKind::Flock, {Fresh, 6}, diagnostic::FileAdvisoryLockProvider);
  EXPECT_EQ(ok(ServiceKind::Flock, {Held, 6}), 0u);
}
TEST_P(DarwinCreatedAdvisoryLockTest,
       PersistentStatusBelongsToEachDescription) {
  for (uint32_t FD : {0u, 1u, 2u}) {
    const uint64_t Initial = FD ? 1 : 0;
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), Initial);
    for (uint64_t Request :
         {uint64_t(0x4000), uint64_t(0x14002), uint64_t(2)}) {
      EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 4, Request}), 0u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), Initial);
    }
    refused(ServiceKind::Flock, {FD, 6}, diagnostic::FileAdvisoryLockKind);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), Initial);
  }
  Options->AdvisoryLocks->Objects.insert("/data");
  reset();
  for (bool Initial : {true, false}) {
    const auto A = Initial ? open() : create();
    const auto B = open(Initial ? "/data" : "/fresh"),
               D = ok(ServiceKind::Dup, {A});
    const uint64_t Access = Initial ? 0 : 2;
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 4, Access | 0x4000}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), Access);
    EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Access);
    error(ServiceKind::Flock, {A, 0}, 9);
    EXPECT_EQ(ok(ServiceKind::Flock, {D, 6}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), Access | 0x4000);
    error(ServiceKind::Flock, {B, 6}, 35);
    refused(ServiceKind::Flock, {B, 2}, diagnostic::FileAdvisoryLockWait);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 0u);
    EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
    for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
      EXPECT_EQ(ok(ServiceKind::Fcntl,
                   {High | D, High | 4, High | (Access | 0x4000)}),
                0u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), Access | 0x4000);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 4, Access}), 0u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Access | 0x4000);
    }
    EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 0x4000u);
    EXPECT_EQ(ok(ServiceKind::Flock, {B, 8}), 0u);
  }
}
TEST_P(DarwinCreatedAdvisoryLockTest,
       CarriersAndCompleteDirtyStateKeepNativeContract) {
  const auto A = create(), B = open("/fresh"), D = ok(ServiceKind::Dup, {A});
  const std::array<uint8_t, 4> Input{'A', 0, 255, 'B'};
  llvm::cantFail(Space->write(Base + Page * 2, Input));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 4}), 4u);
  const auto Before = status(A);
  EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 16), 100u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  for (uint64_t High : {0ULL, 0x1234567800000000ULL, 0xffffffff00000000ULL}) {
    EXPECT_EQ(ok(ServiceKind::Flock, {High | D, High | 6, UINT64_MAX}), 0u);
    error(ServiceKind::Flock, {High | B, High | 5}, 35);
    EXPECT_EQ(status(A), Before);
    EXPECT_EQ(status(B), Before);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 0x14002u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 0u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 4u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Flock, {High | D, High | 8}), 0u);
  }
  EXPECT_EQ(ok(ServiceKind::Pread, {B, Base + Page * 2, 4, 0}), 4u);
  std::array<uint8_t, 4> Output;
  llvm::cantFail(Space->read(Base + Page * 2, Output));
  EXPECT_EQ(Output, Input);
}
TEST_P(DarwinCreatedAdvisoryLockTest,
       DescriptionDuplicatesAndFailuresKeepOneOwner) {
  const auto A = create(), B = open("/fresh"), D = ok(ServiceKind::Dup, {A}),
             F = ok(ServiceKind::Fcntl, {A, 0, 10});
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, 20}), 20u);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  error(ServiceKind::Dup2, {UINT64_MAX, 20}, 9);
  for (auto FD : {A, D, F, uint64_t(20)}) {
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0x4002u);
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    error(ServiceKind::Flock, {FD, 6}, 9);
    if (FD != 20)
      error(ServiceKind::Flock, {B, 6}, 35);
  }
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
  const auto Reused = open();
  EXPECT_EQ(Reused, A);
  refused(ServiceKind::Flock, {Reused, 6},
          diagnostic::FileAdvisoryLockProvider);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Reused, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {B, Reused}), Reused);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Reused, 3}), 0x4000u);
  EXPECT_EQ(ok(ServiceKind::Flock, {Reused, 8}), 0u);
}
TEST_P(DarwinCreatedAdvisoryLockTest, LinkRenameAndZeroNamesKeepObjectGrant) {
  const auto A = create(), B = open("/fresh");
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Link, {path("/fresh"), path("/alias", Base + 128)}),
            0u);
  const auto Alias = open("/alias");
  error(ServiceKind::Flock, {Alias, 6}, 35);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/fresh")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Held = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Held.data() + 14), 0u);
  const auto Fresh = create();
  EXPECT_EQ(ok(ServiceKind::Flock, {Fresh, 6}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(status(A), Held);
  EXPECT_NE(llvm::support::endian::read64le(status(Fresh).data() + 16),
            llvm::support::endian::read64le(Held.data() + 16));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Fresh, 3}), 0x4002u);
}
TEST_P(DarwinCreatedAdvisoryLockTest, MappingLeaseKeepsObjectButReleasesLock) {
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
  const auto Address = (**Mapped).Value,
             Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Flock, {A, 6}, 9);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
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
TEST_P(DarwinCreatedAdvisoryLockTest,
       FailedCreationPreservesBudgetsAndNextInode) {
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
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  Options = darwin_test::createdAdvisoryLockOptions();
  Options->CreationPolicy->FirstInode = UINT64_MAX;
  reset();
  const auto Last = create();
  for (unsigned I = 0; I != 2; ++I)
    refused(ServiceKind::Open, {path("/next"), 0xa02, 0600},
            diagnostic::FileCreationInode);
  EXPECT_EQ(ok(ServiceKind::Flock, {Last, 6}), 0u);
  for (bool Entries : {false, true}) {
    Options = darwin_test::createdAdvisoryLockOptions();
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
    error(ServiceKind::Flock, {4, 6}, 9);
  }
}
TEST_P(DarwinCreatedAdvisoryLockTest,
       DirectoriesAndLinksNeverAcquireRegularGrant) {
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::Symlink, {path("target"), path("/link", Base + 128)}),
      0u);
  const auto D = open("/dir", 0x100000), L = open("/link", 0x200000);
  for (auto FD : {D, L})
    refused(ServiceKind::Flock, {FD, 6}, diagnostic::FileAdvisoryLockTarget);
  for (auto FD : {0u, 1u, 2u})
    refused(ServiceKind::Flock, {FD, 6}, diagnostic::FileAdvisoryLockKind);
  const auto A = create();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
}
TEST_P(DarwinCreatedAdvisoryLockTest, OtherAuthoritiesRemainIndependent) {
  const auto A = create();
  const auto Before = status(A);
  refused(ServiceKind::Fsync, {A}, diagnostic::FileSynchronizationEnvironment);
  refused(ServiceKind::Fdatasync, {A},
          diagnostic::FileDataSynchronizationEnvironment);
  refused(ServiceKind::Fchmod, {A, 0600}, diagnostic::FileModeEnvironment);
  refused(ServiceKind::Fchown, {A, 501, 20},
          diagnostic::FileOwnershipEnvironment);
  EXPECT_FALSE(invoke(ServiceKind::FgetXattr,
                      {A, path("tag", Base + 128), Base + Page * 2, 4, 0, 0}));
  const auto Reason = Result.Diagnostic;
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::FgetXattr,
                      {A, path("tag", Base + 128), Base + Page * 2, 4, 0, 0}));
  EXPECT_EQ(Result.Diagnostic, Reason);
  EXPECT_EQ(status(A), Before);
}

INSTANTIATE_TEST_SUITE_P(Pages, DarwinCreatedAdvisoryLockTest,
                         testing::Values(uint64_t(4096), uint64_t(16384)));
} // namespace
} // namespace neverd::emulation::darwin_model
