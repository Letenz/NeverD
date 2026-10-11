//===- DarwinSynchronizationTests.cpp - File completion contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original file synchronization provider and object lifetime regressions.
#include "DarwinSynchronizationTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinSynchronizationTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "memory-only file synchronization"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::synchronizationOptions();
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
        ExecutionBackendKind::Unicorn, "memory-only file synchronization"};
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
TEST(DarwinSynchronizationOptions, CompleteOrdinaryObjectsAndQueryClosure) {
  const auto Original = darwin_test::synchronizationOptions();
  EXPECT_FALSE(bool(validateFileOptions(Original)));
  for (unsigned I = 0; I != 10; ++I) {
    auto O = Original;
    switch (I) {
    case 0:
      O.Synchronization->Objects.insert("/missing");
      break;
    case 1:
      O.Synchronization->Objects.insert("/");
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
      O.Synchronization->Objects.insert("relative");
      break;
    case 8:
      O.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
      O.Synchronization->Objects.insert("/link");
      break;
    case 9:
      O.Synchronization->Objects.insert("/data/../data");
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
    O.Synchronization->Objects.clear();
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST(DarwinSynchronizationOptions, ReferencesShareTheInputByteBudget) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 6);
  O.Metadata["/a"] =
      darwin_test::modeMetadata(0100000, 1, O.Files["/a"].size());
  O.Synchronization.emplace().Objects.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files["/a"].resize(darwin_file_limits::Bytes - 15);
  O.Metadata["/a"].Size = O.Files["/a"].size();
  O.ModeMutations.emplace().Objects["/a"] = {1, 2};
  O.OwnershipMutations.emplace().Objects["/a"] = {3, 4};
  O.AdvisoryLocks.emplace().Objects.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST_P(DarwinSynchronizationTest,
       AccessFlagsCarriersAndWholeStateRemainIndependent) {
  mutationPolicies();
  for (uint32_t Flags : {0u, 1u, 2u}) {
    const auto A = open("/data", Flags), D = ok(ServiceKind::Dup, {A});
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 2, 0}), 2u);
    const auto Before = status(A);
    const auto FileFlags = ok(ServiceKind::Fcntl, {A, 3});
    for (uint64_t High :
         {uint64_t(0), uint64_t(0x1234567800000000), UINT64_MAX << 32}) {
      EXPECT_EQ(ok(ServiceKind::Fsync, {High | A, UINT64_MAX, UINT64_MAX,
                                        UINT64_MAX, UINT64_MAX, UINT64_MAX}),
                0u);
      EXPECT_EQ(ok(ServiceKind::Fsync, {High | D}), 0u);
      EXPECT_EQ(status(A), Before);
      EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), FileFlags);
    }
    EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
    error(ServiceKind::Fsync, {A}, 9);
    EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
    EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  }
  EXPECT_EQ(Options->Files.at("/data"),
            (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinSynchronizationTest, BadFDPrecedesKindAndAbsentEnvironment) {
  Options.reset();
  reset();
  error(ServiceKind::Fsync, {UINT64_MAX, Base}, 9);
  error(ServiceKind::Fsync, {0x12345678ffffffffULL}, 9);
  refused(ServiceKind::Fsync, {1}, diagnostic::FileSynchronizationKind);
}
TEST_P(DarwinSynchronizationTest, MissingAndExplicitEmptyProvidersDiffer) {
  Options->Synchronization.reset();
  reset();
  const auto A = open();
  const auto Before = status(A);
  refused(ServiceKind::Fsync, {A}, diagnostic::FileSynchronizationEnvironment);
  EXPECT_EQ(status(A), Before);
  Options->Synchronization.emplace();
  reset();
  const auto B = open();
  refused(ServiceKind::Fsync, {B}, diagnostic::FileSynchronizationProvider);
  error(ServiceKind::Fsync, {UINT64_MAX}, 9);
}
TEST_P(DarwinSynchronizationTest,
       KnownNonRegularVnodesNeedIndependentProvider) {
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  reset();
  const auto Directory = open("/", 0), Link = open("/link", 0x200000);
  refused(ServiceKind::Fsync, {Directory},
          diagnostic::FileSynchronizationTarget);
  refused(ServiceKind::Fsync, {Link}, diagnostic::FileSynchronizationTarget);
  const auto Referent = open("/link");
  EXPECT_EQ(ok(ServiceKind::Fsync, {Referent}), 0u);
  EXPECT_EQ(status(Referent), status(open()));
}
TEST_P(DarwinSynchronizationTest,
       RuntimeNamesKeepOriginalGrantAndRecreationDoesNot) {
  mutationPolicies();
  const auto A = open();
  EXPECT_EQ(ok(ServiceKind::Link, {path("/data"), path("/alias", Base + 128)}),
            0u);
  const auto Alias = open("/alias");
  EXPECT_EQ(ok(ServiceKind::Fsync, {Alias}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Before = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Before.data() + 14), 0u);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fsync, {Alias}), 0u);
  EXPECT_EQ(status(A), Before);
  const auto New = open("/data", 0x202);
  const auto Created = status(New);
  refused(ServiceKind::Fsync, {New}, diagnostic::FileSynchronizationProvider);
  EXPECT_EQ(status(New), Created);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(Options->Synchronization->Objects,
            (std::set<std::string>{"/data"}));
}
TEST_P(DarwinSynchronizationTest,
       FcntlDup2AndFDReuseSelectCurrentContentsGrant) {
  Options->Files["/other"] = Options->Files["/data"];
  Options->Metadata["/other"] = darwin_test::modeMetadata(0100600, 3, 6);
  reset();
  const auto A = open(), Other = open("/other"),
             D = ok(ServiceKind::Fcntl, {A, 0, 10});
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {Other, D}), D);
  refused(ServiceKind::Fsync, {D}, diagnostic::FileSynchronizationProvider);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, D}), D);
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  const auto Reused = open("/other");
  EXPECT_EQ(Reused, A);
  refused(ServiceKind::Fsync, {Reused},
          diagnostic::FileSynchronizationProvider);
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
}
TEST_P(DarwinSynchronizationTest,
       RealVMAndRetainedContentsCannotMakeClosedFDValid) {
  const auto A = open(), D = ok(ServiceKind::Dup, {A});
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
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Fsync, {A}, 9);
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  error(ServiceKind::Fsync, {D}, 9);
  std::array<uint8_t, 6> Bytes;
  ASSERT_FALSE(bool(Space->read(Address, Bytes)));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_GT(std::get<DarwinFiles::Mapping>(Snapshot).Lease.use_count(), 1);
  auto Unmapped = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}},
                            *Files, Result);
  ASSERT_TRUE(bool(Unmapped)) << llvm::toString(Unmapped.takeError());
  ASSERT_TRUE(*Unmapped);
  EXPECT_FALSE((**Unmapped).Error);
}
TEST_P(DarwinSynchronizationTest,
       CommittedWritesAndTruncateKeepCurrentStatAndCursor) {
  mutationPolicies();
  const auto A = open("/data", 2), D = ok(ServiceKind::Dup, {A});
  const std::array<uint8_t, 2> Bytes{'X', 'Y'};
  llvm::cantFail(Space->write(Base + Page * 2, Bytes));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 2}), 2u);
  const auto Written = status(A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
  EXPECT_EQ(status(A), Written);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 3}), 0u);
  const auto Truncated = status(A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(status(D), Truncated);
  std::array<uint8_t, 3> Observed;
  EXPECT_EQ(ok(ServiceKind::Pread, {D, Base + Page * 2, 3, 0}), 3u);
  llvm::cantFail(Space->read(Base + Page * 2, Observed));
  EXPECT_EQ(Observed, (std::array<uint8_t, 3>{'X', 'Y', 0}));
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(Options->Files.at("/data"),
            (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinSynchronizationTest,
       CompletionCannotRestoreUnknownMetadataOrAttributes) {
  Options->WritableFiles.insert("/data");
  Options->ExtendedAttributes["/data"] = {{"tag", {1, 2}}};
  reset();
  const auto A = open("/data", 2);
  const std::array<uint8_t, 1> Bytes{'X'};
  llvm::cantFail(Space->write(Base + Page * 2, Bytes));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 1}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, Base + Page + 8}));
  const auto Unknown = Result.Diagnostic;
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, Base + Page + 8}));
  EXPECT_EQ(Result.Diagnostic, Unknown);
  const auto Name = path("tag", Base + 128);
  EXPECT_FALSE(
      invoke(ServiceKind::FgetXattr, {A, Name, Base + Page * 2, 2, 0, 0}));
  const auto AttributeUnknown = Result.Diagnostic;
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_FALSE(
      invoke(ServiceKind::FgetXattr, {A, Name, Base + Page * 2, 2, 0, 0}));
  EXPECT_EQ(Result.Diagnostic, AttributeUnknown);
}
TEST_P(DarwinSynchronizationTest,
       ModeAndOwnershipRetainIndependentCurrentRecord) {
  Options->ModeMutations.emplace().Objects["/data"] = {11, 12};
  Options->OwnershipMutations.emplace().Objects["/data"] = {13, 14};
  reset();
  const auto A = open();
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0640}), 0u);
  const auto Mode = status(A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(status(A), Mode);
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, UINT32_MAX, UINT32_MAX}), 0u);
  const auto Owner = status(A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(status(A), Owner);
}
TEST_P(DarwinSynchronizationTest, CompletionNeitherAcquiresNorReleasesLocks) {
  Options->AdvisoryLocks.emplace().Objects.insert("/data");
  reset();
  const auto A = open(), B = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Fsync, {B}), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
}
TEST_P(DarwinSynchronizationTest, OtherGrantsCannotSupplyCompletionProvider) {
  mutationPolicies();
  Options->Synchronization.reset();
  Options->AdvisoryLocks.emplace().Objects.insert("/data");
  Options->ModeMutations.emplace().Objects["/data"] = {11, 12};
  Options->OwnershipMutations.emplace().Objects["/data"] = {13, 14};
  reset();
  const auto A = open();
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  refused(ServiceKind::Fsync, {A}, diagnostic::FileSynchronizationEnvironment);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
}
INSTANTIATE_TEST_SUITE_P(Pages, DarwinSynchronizationTest,
                         testing::Values(uint64_t(4096), uint64_t(16384)));
} // namespace
} // namespace neverd::emulation::darwin_model
