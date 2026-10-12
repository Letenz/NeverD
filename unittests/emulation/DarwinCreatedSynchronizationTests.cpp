//===- DarwinCreatedSynchronizationTests.cpp - Data completion contracts
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original created completion provider and object lifetime regressions.
#include "DarwinCreatedSynchronizationTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinCreatedSynchronizationTest
    : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "memory-only created synchronization"};
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::createdSynchronizationOptions();
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
        ExecutionBackendKind::Unicorn, "memory-only created synchronization"};
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
TEST(DarwinCreatedSynchronizationOptions,
     DefaultsRequireCompleteCreationPolicy) {
  EXPECT_FALSE(DarwinFileSynchronizationOptions{}.CreatedRegularObjects);
  EXPECT_FALSE(DarwinFileDataSynchronizationOptions{}.CreatedRegularObjects);
  for (bool Data : {false, true}) {
    auto O = darwin_test::createdSynchronizationOptions();
    O.CreationPolicy.reset();
    O.Synchronization->CreatedRegularObjects = !Data;
    O.DataSynchronization->CreatedRegularObjects = Data;
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
    O.Synchronization->CreatedRegularObjects = false;
    O.DataSynchronization->CreatedRegularObjects = false;
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  for (unsigned I = 0; I != 8; ++I) {
    auto O = darwin_test::createdSynchronizationOptions();
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
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << I;
    llvm::consumeError(std::move(E));
  }
}
TEST(DarwinCreatedSynchronizationOptions,
     CreationCannotBypassInitialAdmissionOrSharedBudget) {
  for (bool Data : {false, true}) {
    auto O = darwin_test::createdSynchronizationOptions();
    if (Data)
      O.DataSynchronization->Objects.insert("/data");
    else
      O.Synchronization->Objects.insert("/data");
    O.Metadata["/data"].Flags = 1;
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    O.Metadata["/data"].Flags = 0;
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  auto O = darwin_test::createdSynchronizationOptions();
  O.Files.clear();
  O.Metadata.erase("/data");
  // Root metadata, mutable root, mutation policy and CWD each reserve two
  // bytes.
  O.StandardInput = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.StandardInput->push_back(0);
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DarwinCreatedSynchronizationOptions,
     StaticQueriesRejectEvenFalseDeclarations) {
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries})
    for (bool Data : {false, true}) {
      DarwinFileOptions O;
      O.Authorization = A;
      if (Data)
        O.DataSynchronization.emplace();
      else
        O.Synchronization.emplace();
      auto E = validateFileOptions(O);
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    }
}
TEST_P(DarwinCreatedSynchronizationTest, EachPurposeHasIndependentDefaults) {
  for (bool File : {false, true})
    for (bool Data : {false, true}) {
      Options = darwin_test::createdSynchronizationOptions(File, Data);
      reset();
      const auto A = create();
      const auto Before = status(A);
      if (File)
        EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
      else
        refused(ServiceKind::Fsync, {A},
                diagnostic::FileSynchronizationProvider);
      if (Data)
        EXPECT_EQ(ok(ServiceKind::Fdatasync, {A}), 0u);
      else
        refused(ServiceKind::Fdatasync, {A},
                diagnostic::FileDataSynchronizationProvider);
      EXPECT_EQ(status(A), Before);
    }
  Options = darwin_test::createdSynchronizationOptions();
  Options->Synchronization.reset();
  Options->DataSynchronization.reset();
  reset();
  const auto A = create();
  refused(ServiceKind::Fsync, {A}, diagnostic::FileSynchronizationEnvironment);
  refused(ServiceKind::Fdatasync, {A},
          diagnostic::FileDataSynchronizationEnvironment);
  error(ServiceKind::Fsync, {UINT64_MAX}, 9);
  error(ServiceKind::Fdatasync, {UINT64_MAX}, 9);
}
TEST_P(DarwinCreatedSynchronizationTest,
       InitialAndCreatedGrantsDoNotSupplyEachOther) {
  const auto Initial = open();
  refused(ServiceKind::Fsync, {Initial},
          diagnostic::FileSynchronizationProvider);
  refused(ServiceKind::Fdatasync, {Initial},
          diagnostic::FileDataSynchronizationProvider);
  const auto Created = create();
  EXPECT_EQ(ok(ServiceKind::Fsync, {Created}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {Created}), 0u);
  Options = darwin_test::createdSynchronizationOptions(false, false);
  Options->Synchronization->Objects.insert("/data");
  Options->DataSynchronization->Objects.insert("/data");
  reset();
  const auto Old = open(), New = create();
  EXPECT_EQ(ok(ServiceKind::Fsync, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {Old}), 0u);
  refused(ServiceKind::Fsync, {New}, diagnostic::FileSynchronizationProvider);
  refused(ServiceKind::Fdatasync, {New},
          diagnostic::FileDataSynchronizationProvider);
}
TEST_P(DarwinCreatedSynchronizationTest,
       CompleteDirtyStateAndCarriersStayUnchanged) {
  const auto A = create("/fresh", 07777), D = ok(ServiceKind::Dup, {A});
  auto S = status(A);
  auto U16 = [&](unsigned Offset) {
    return llvm::support::endian::read16le(S.data() + 8 + Offset);
  };
  auto U32 = [&](unsigned Offset) {
    return llvm::support::endian::read32le(S.data() + 8 + Offset);
  };
  auto U64 = [&](unsigned Offset) {
    return llvm::support::endian::read64le(S.data() + 8 + Offset);
  };
  EXPECT_EQ(U32(0), 7u);
  EXPECT_EQ(U16(4), 0100755u);
  EXPECT_EQ(U16(6), 1u);
  EXPECT_EQ(U64(8), 100u);
  EXPECT_EQ(U32(16), 777u);
  EXPECT_EQ(U32(20), 20u);
  for (unsigned Offset : {32u, 48u, 64u, 80u}) {
    EXPECT_EQ(U64(Offset), 41u);
    EXPECT_EQ(U64(Offset + 8), 42u);
  }
  EXPECT_EQ(U64(96), 0u);
  EXPECT_EQ(U64(104), 0u);
  EXPECT_EQ(U32(112), 512u);
  EXPECT_EQ(U32(116), 0u);
  EXPECT_EQ(U32(120), 9u);
  const std::array<uint8_t, 4> Bytes{'A', 0, 255, 'B'};
  llvm::cantFail(Space->write(Base + Page * 2, Bytes));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, Bytes.size()}), 4u);
  const auto Written = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  for (uint64_t High :
       {uint64_t(0), uint64_t(0x1234567800000000), UINT64_MAX << 32})
    for (auto K : {ServiceKind::Fsync, ServiceKind::Fdatasync}) {
      EXPECT_EQ(ok(K, {High | D, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
                       UINT64_MAX}),
                0u);
      EXPECT_EQ(status(A), Written);
      EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 4u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Flags);
    }
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 3}), 0u);
  const auto Truncated = status(A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {D}), 0u);
  EXPECT_EQ(status(A), Truncated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 4u);
  std::array<uint8_t, 3> Read;
  EXPECT_EQ(ok(ServiceKind::Pread, {D, Base + Page * 2, 3, 0}), 3u);
  llvm::cantFail(Space->read(Base + Page * 2, Read));
  EXPECT_EQ(Read, (std::array<uint8_t, 3>{'A', 0, 255}));
  EXPECT_EQ(Options->Files.at("/data"),
            (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinCreatedSynchronizationTest,
       RuntimeNamesAndReplacementKeepSeparateObjectGrants) {
  Options->DataSynchronization->CreatedRegularObjects = false;
  Options->DataSynchronization->Objects.insert("/data");
  reset();
  const auto A = create(), D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Link, {path("/fresh"), path("/alias", Base + 128)}),
            0u);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/fresh"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/alias")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Held = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Held.data() + 14), 0u);
  const auto New = create();
  const auto Fresh = status(New);
  EXPECT_NE(llvm::support::endian::read64le(Held.data() + 16),
            llvm::support::endian::read64le(Fresh.data() + 16));
  for (auto FD : {A, D, New}) {
    EXPECT_EQ(ok(ServiceKind::Fsync, {FD}), 0u);
    refused(ServiceKind::Fdatasync, {FD},
            diagnostic::FileDataSynchronizationProvider);
  }
  EXPECT_EQ(status(A), Held);
  const auto Old = open();
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  const auto Replacement = create("/data");
  refused(ServiceKind::Fdatasync, {Replacement},
          diagnostic::FileDataSynchronizationProvider);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fsync, {Replacement}), 0u);
}
TEST_P(DarwinCreatedSynchronizationTest,
       DuplicatesAndFDReuseSelectTheCurrentContents) {
  const auto A = create(), D = ok(ServiceKind::Fcntl, {A, 0, 10}), Old = open();
  EXPECT_EQ(D, 10u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {Old, A}), A);
  refused(ServiceKind::Fsync, {A}, diagnostic::FileSynchronizationProvider);
  refused(ServiceKind::Fdatasync, {A},
          diagnostic::FileDataSynchronizationProvider);
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Fsync, {A}, 9);
  error(ServiceKind::Fdatasync, {A}, 9);
  const auto Reused = create("/next");
  EXPECT_EQ(Reused, A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {Reused}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {Reused}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {D, 20}), 20u);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  error(ServiceKind::Fdatasync, {D}, 9);
  EXPECT_EQ(ok(ServiceKind::Fsync, {20}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {20}), 0u);
}
TEST_P(DarwinCreatedSynchronizationTest,
       RealMappingCannotReviveClosedDescriptors) {
  const auto A = create(), D = ok(ServiceKind::Dup, {A});
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
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Fsync, {A}, 9);
  error(ServiceKind::Fdatasync, {A}, 9);
  EXPECT_EQ(ok(ServiceKind::Fsync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  error(ServiceKind::Fsync, {D}, 9);
  error(ServiceKind::Fdatasync, {D}, 9);
  std::array<uint8_t, 3> Bytes;
  ASSERT_FALSE(bool(Space->read(Address, Bytes)));
  EXPECT_EQ(Bytes, Input);
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_GT(std::get<DarwinFiles::Mapping>(Snapshot).Lease.use_count(), 1);
  auto Unmapped = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}},
                            *Files, Result);
  ASSERT_TRUE(bool(Unmapped));
  ASSERT_TRUE(*Unmapped);
  EXPECT_FALSE((**Unmapped).Error);
}
TEST_P(DarwinCreatedSynchronizationTest,
       FailedDescriptorAndInodeAdmissionPublishesNothing) {
  Options->DescriptorLimit = 4;
  reset();
  const auto Old = open();
  const auto Before = status(Old);
  for (unsigned I = 0; I != 2; ++I)
    error(ServiceKind::Open, {path("/fresh"), 0xa02, 0600}, 24);
  EXPECT_EQ(status(Old), Before);
  refused(ServiceKind::Fsync, {Old}, diagnostic::FileSynchronizationProvider);
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  const auto A = create();
  EXPECT_EQ(A, Old);
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 16), 100u);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  Options = darwin_test::createdSynchronizationOptions();
  Options->CreationPolicy->FirstInode = UINT64_MAX;
  reset();
  const auto Last = create();
  const auto Final = status(Last);
  for (unsigned I = 0; I != 2; ++I)
    refused(ServiceKind::Open, {path("/next"), 0xa02, 0600},
            diagnostic::FileCreationInode);
  EXPECT_EQ(status(Last), Final);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {Last}), 0u);
}
TEST_P(DarwinCreatedSynchronizationTest,
       FailedStorageAndEntryAdmissionPreservesState) {
  Options->Files.clear();
  Options->Metadata.erase("/data");
  Options->StandardInput = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  reset();
  const auto Root = open("/", 0x100000);
  const auto Before = status(Root);
  for (unsigned I = 0; I != 2; ++I)
    refused(ServiceKind::Open, {path("/fresh"), 0xa02, 0600},
            diagnostic::FileCreationLimit);
  EXPECT_EQ(status(Root), Before);
  error(ServiceKind::Fdatasync, {4}, 9);
  Options = darwin_test::createdSynchronizationOptions();
  Options->Files.clear();
  Options->Metadata.erase("/data");
  for (unsigned I = 0; I != darwin_file_limits::Files - 1; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  reset();
  const auto Dir = open("/", 0x100000);
  const auto Initial = status(Dir);
  for (unsigned I = 0; I != 2; ++I)
    refused(ServiceKind::Open, {path("/fresh"), 0xa02, 0600},
            diagnostic::FileCreationLimit);
  EXPECT_EQ(status(Dir), Initial);
  error(ServiceKind::Fsync, {4}, 9);
}
TEST_P(DarwinCreatedSynchronizationTest,
       DirectoryAndLinkCreationNeverGrantsRegularCompletion) {
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/dir"), 0700}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::Symlink, {path("target"), path("/link", Base + 128)}),
      0u);
  const auto D = open("/dir", 0x100000), L = open("/link", 0x200000);
  for (auto FD : {D, L}) {
    refused(ServiceKind::Fsync, {FD}, diagnostic::FileSynchronizationTarget);
    refused(ServiceKind::Fdatasync, {FD},
            diagnostic::FileDataSynchronizationTarget);
  }
  const auto A = create();
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {A}), 0u);
}
TEST_P(DarwinCreatedSynchronizationTest,
       OtherAuthoritiesAndUnknownObservationsStayIndependent) {
  const auto A = create();
  const auto Before = status(A);
  refused(ServiceKind::Fchmod, {A, 0600}, diagnostic::FileModeEnvironment);
  refused(ServiceKind::Fchown, {A, 501, 20},
          diagnostic::FileOwnershipEnvironment);
  refused(ServiceKind::Flock, {A, 8}, diagnostic::FileAdvisoryLockEnvironment);
  EXPECT_FALSE(invoke(ServiceKind::FgetXattr,
                      {A, path("tag", Base + 128), Base + Page * 2, 4, 0, 0}));
  const auto Reason = Result.Diagnostic;
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {A}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::FgetXattr,
                      {A, path("tag", Base + 128), Base + Page * 2, 4, 0, 0}));
  EXPECT_EQ(Result.Diagnostic, Reason);
  EXPECT_EQ(status(A), Before);
  const auto Initial = open();
  refused(ServiceKind::Fdatasync, {Initial},
          diagnostic::FileDataSynchronizationProvider);
  const auto Existing = ok(ServiceKind::Open, {path("/data"), 0x200, 0600});
  refused(ServiceKind::Fsync, {Existing},
          diagnostic::FileSynchronizationProvider);
  error(ServiceKind::Open, {path("/data"), 0xa02, 0600}, 17);
  EXPECT_EQ(status(Initial), status(Existing));
}
INSTANTIATE_TEST_SUITE_P(Pages, DarwinCreatedSynchronizationTest,
                         testing::Values(uint64_t(4096), uint64_t(16384)));
} // namespace
} // namespace neverd::emulation::darwin_model
