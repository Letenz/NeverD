//===- DarwinFullSynchronizationTests.cpp - Full/barrier completion ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original independent provider, error order and retained-object regressions.
#include "DarwinFullSynchronizationTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

#include "llvm/Support/Endian.h"
namespace neverd::emulation::darwin_model {
namespace {
class DarwinFullSynchronizationTest
    : public testing::TestWithParam<std::tuple<uint64_t, uint32_t>> {
protected:
  static constexpr uint64_t Base = 0x100000;
  uint64_t Page;
  uint32_t Command;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::optional<DarwinCredentials> Credentials;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "full/barrier completion"};
  void SetUp() override {
    std::tie(Page, Command) = GetParam();
    auto Physical = PhysicalMemory::create(Page * 16);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 16);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 4, Read | Write | UserAccessible)));
    Options = darwin_test::fullSynchronizationOptions();
    Credentials = darwin_test::modeCredentials();
    reset();
  }
  void reset() {
    if (Options)
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(
        *Space, Options, process_defaults::Output, 501, Credentials);
  }
  std::set<std::string> &objects() {
    return Command == 51 ? Options->FullSynchronization->Objects
                         : Options->BarrierSynchronization->Objects;
  }
  void drop() {
    if (Command == 51)
      Options->FullSynchronization.reset();
    else
      Options->BarrierSynchronization.reset();
  }
  void declare() {
    if (Command == 51)
      Options->FullSynchronization.emplace();
    else
      Options->BarrierSynchronization.emplace();
  }
  const char *environment() const {
    return Command == 51 ? diagnostic::FileFullSynchronizationEnvironment
                         : diagnostic::FileBarrierSynchronizationEnvironment;
  }
  const char *provider() const {
    return Command == 51 ? diagnostic::FileFullSynchronizationProvider
                         : diagnostic::FileBarrierSynchronizationProvider;
  }
  const char *kind() const {
    return Command == 51 ? diagnostic::FileFullSynchronizationKind
                         : diagnostic::FileBarrierSynchronizationKind;
  }
  const char *target() const {
    return Command == 51 ? diagnostic::FileFullSynchronizationTarget
                         : diagnostic::FileBarrierSynchronizationTarget;
  }
  uint64_t path(llvm::StringRef Text, uint64_t Address = Base) {
    const auto Bytes = Text.str() + '\0';
    llvm::cantFail(Space->write(
        Address,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Bytes.data()),
                                Bytes.size())));
    return Address;
  }
  std::optional<ServiceResult> invoke(ServiceKind K,
                                      std::array<uint64_t, 6> A) {
    Result =
        ProcessResult{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                      ExecutionBackendKind::Unicorn, "full/barrier completion"};
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
  uint64_t complete(uint64_t FD, uint64_t High = 0,
                    uint64_t Argument = UINT64_MAX) {
    return ok(ServiceKind::Fcntl, {FD, High | Command, Argument});
  }
  void refusedCompletion(uint64_t FD, const char *Reason) {
    refused(ServiceKind::Fcntl, {FD, Command, UINT64_MAX}, Reason);
  }
  uint64_t open(llvm::StringRef Name = "/data", uint32_t Flags = 0) {
    return ok(ServiceKind::Open, {path(Name), Flags, 0600});
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
    P.Generation = 9;
    P.Time = {41, 42};
    P.Mutation = {512, {101, 102}};
    P.Namespace = DarwinNamespaceCreationPolicy{512, 16, 0};
    Options->CreationPolicy = P;
    reset();
  }
};
TEST(DarwinFullSynchronizationOptions,
     DefaultsInitialAdmissionAndQueryClosure) {
  const auto Original = darwin_test::fullSynchronizationOptions();
  EXPECT_FALSE(Original.FullSynchronization->CreatedRegularObjects);
  EXPECT_FALSE(Original.BarrierSynchronization->CreatedRegularObjects);
  EXPECT_FALSE(bool(validateFileOptions(Original)));
  for (bool Full : {false, true})
    for (unsigned I = 0; I != 11; ++I) {
      auto O = Original;
      auto &Objects = Full ? O.FullSynchronization->Objects
                           : O.BarrierSynchronization->Objects;
      switch (I) {
      case 0:
        Objects.insert("/missing");
        break;
      case 1:
        Objects.insert("/");
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
        Objects.insert("relative");
        break;
      case 8:
        O.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
        Objects.insert("/link");
        break;
      case 9:
        Objects.insert("/data/../data");
        break;
      case 10:
        Objects.insert(std::string("/data\0tail", 10));
        break;
      }
      auto E = validateFileOptions(O);
      EXPECT_TRUE(bool(E)) << Full << ':' << I;
      llvm::consumeError(std::move(E));
    }
  for (auto A : {DarwinFileAuthorization::StaticOwnerQueries,
                 DarwinFileAuthorization::StaticOrdinaryQueries})
    for (bool Full : {false, true}) {
      DarwinFileOptions O;
      O.Authorization = A;
      if (Full)
        O.FullSynchronization.emplace();
      else
        O.BarrierSynchronization.emplace();
      auto E = validateFileOptions(O);
      EXPECT_TRUE(bool(E));
      llvm::consumeError(std::move(E));
    }
}
TEST(DarwinFullSynchronizationOptions, BothProvidersShareReferenceByteBudget) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 9);
  O.Metadata["/a"] =
      darwin_test::modeMetadata(0100600, 1, O.Files["/a"].size());
  O.FullSynchronization.emplace().Objects.insert("/a");
  O.BarrierSynchronization.emplace().Objects.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files["/a"].resize(darwin_file_limits::Bytes - 24);
  O.Metadata["/a"].Size = O.Files["/a"].size();
  O.ModeMutations.emplace().Objects["/a"] = {1, 2};
  O.OwnershipMutations.emplace().Objects["/a"] = {3, 4};
  O.AdvisoryLocks.emplace().Objects.insert("/a");
  O.Synchronization.emplace().Objects.insert("/a");
  O.DataSynchronization.emplace().Objects.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  ++O.Metadata["/a"].Size;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}
TEST(DarwinFullSynchronizationOptions,
     CreationFlagsRequireCompletePolicyIndependently) {
  for (bool Full : {false, true}) {
    auto O = darwin_test::fullSynchronizationOptions();
    if (Full)
      O.FullSynchronization->CreatedRegularObjects = true;
    else
      O.BarrierSynchronization->CreatedRegularObjects = true;
    auto E = validateFileOptions(O);
    ASSERT_TRUE(bool(E));
    EXPECT_EQ(llvm::toString(std::move(E)), diagnostic::FileCreationPolicy);
    O.MutableDirectories.insert("/");
    O.DirectoryMutationPolicies["/"] = {16, {91, 92}};
    O.InitialUmask = 022;
    DarwinFileCreationPolicy P;
    P.FirstInode = 100;
    P.BlockSize = 512;
    P.Generation = 9;
    P.Time = {41, 42};
    P.Mutation = {512, {101, 102}};
    O.CreationPolicy = P;
    EXPECT_FALSE(bool(validateFileOptions(O)));
    O.CreationPolicy->Mutation.AllocationUnit = 0;
    E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
TEST_P(DarwinFullSynchronizationTest,
       CarriersIgnoredArgumentAccessAndWholeState) {
  Options->WritableFiles.insert("/data");
  reset();
  for (uint32_t Flags : {0u, 1u, 2u}) {
    const auto A = open("/data", Flags), D = ok(ServiceKind::Dup, {A});
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 2, 0}), 2u);
    const auto Before = status(A);
    const auto FileFlags = ok(ServiceKind::Fcntl, {A, 3});
    EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 16), 2u);
    EXPECT_EQ(llvm::support::endian::read32le(Before.data() + 24), 501u);
    for (uint64_t High :
         {uint64_t(0), uint64_t(0x1234567800000000), UINT64_MAX << 32}) {
      EXPECT_EQ(complete(High | A, High, UINT64_MAX), 0u);
      EXPECT_EQ(complete(D, High, Base + Page * 15), 0u);
      EXPECT_EQ(status(A), Before);
      EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), FileFlags);
    }
    EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
    error(ServiceKind::Fcntl, {A, Command, UINT64_MAX}, 9);
    EXPECT_EQ(complete(D), 0u);
    EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  }
  EXPECT_EQ(Options->Files.at("/data"),
            (std::vector<uint8_t>{'a', 'b', 0, 255, 'e', 'f'}));
}
TEST_P(DarwinFullSynchronizationTest, BadFDBeforeUnknownKindAndEnvironment) {
  Options.reset();
  reset();
  error(ServiceKind::Fcntl, {UINT64_MAX, Command, Base}, 9);
  error(ServiceKind::Fcntl,
        {0x12345678ffffffffULL, 0xffffffff00000000ULL | Command, UINT64_MAX},
        9);
  refusedCompletion(1, kind());
  refusedCompletion(0, kind());
}
TEST_P(DarwinFullSynchronizationTest,
       MissingAndExplicitEmptyDeclarationsDiffer) {
  drop();
  reset();
  const auto A = open();
  const auto Before = status(A);
  refusedCompletion(A, environment());
  EXPECT_EQ(status(A), Before);
  declare();
  reset();
  const auto B = open();
  refusedCompletion(B, provider());
  EXPECT_EQ(status(B), Before);
  error(ServiceKind::Fcntl, {UINT64_MAX, Command, UINT64_MAX}, 9);
}
TEST_P(DarwinFullSynchronizationTest,
       FileDataAndOtherCommandCannotSupplyGrant) {
  Options->Synchronization.emplace().Objects.insert("/data");
  Options->DataSynchronization.emplace().Objects.insert("/data");
  objects().clear();
  reset();
  const auto A = open();
  const auto Before = status(A);
  EXPECT_EQ(ok(ServiceKind::Fsync, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fdatasync, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, Command == 51 ? 85u : 51u, UINT64_MAX}),
            0u);
  refusedCompletion(A, provider());
  EXPECT_EQ(status(A), Before);
  drop();
  reset();
  refusedCompletion(open(), environment());
}
TEST_P(DarwinFullSynchronizationTest,
       KnownDirectoryAndSymbolicTargetsRemainSeparate) {
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  reset();
  refusedCompletion(open("/"), target());
  refusedCompletion(open("/link", 0x200000), target());
  EXPECT_EQ(complete(open("/link")), 0u);
  refused(ServiceKind::Fcntl, {open(), 52, UINT64_MAX},
          diagnostic::FileControl);
}
TEST_P(DarwinFullSynchronizationTest,
       DuplicateReplacementReuseSelectActualContents) {
  Options->Files["/other"] = Options->Files["/data"];
  Options->Metadata["/other"] = darwin_test::modeMetadata(0100600, 3, 6);
  reset();
  const auto A = open(), Other = open("/other"),
             D = ok(ServiceKind::Fcntl, {A, 0, 10});
  EXPECT_EQ(complete(D), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {Other, D}), D);
  refusedCompletion(D, provider());
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, D}), D);
  EXPECT_EQ(complete(D), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  const auto Reused = open("/other");
  EXPECT_EQ(Reused, A);
  refusedCompletion(Reused, provider());
  EXPECT_EQ(complete(D), 0u);
}
TEST_P(DarwinFullSynchronizationTest,
       LinksRenameHeldUnlinkAndFreshReplacement) {
  mutationPolicies();
  const auto A = open();
  EXPECT_EQ(ok(ServiceKind::Link, {path("/data"), path("/alias", Base + 128)}),
            0u);
  const auto Alias = open("/alias");
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/alias"), path("/moved", Base + 128)}),
      0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/data")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  const auto Held = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Held.data() + 14), 0u);
  EXPECT_EQ(complete(A), 0u);
  EXPECT_EQ(complete(Alias), 0u);
  EXPECT_EQ(status(A), Held);
  const auto Fresh = open("/data", 0x202);
  refusedCompletion(Fresh, provider());
  EXPECT_NE(llvm::support::endian::read64le(status(Fresh).data() + 16), 2u);
  EXPECT_EQ(complete(A), 0u);
}
TEST_P(DarwinFullSynchronizationTest,
       ActualPrivateMappingDoesNotKeepClosedFDValid) {
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
  ASSERT_TRUE(bool(Mapped));
  ASSERT_TRUE(*Mapped);
  ASSERT_FALSE((**Mapped).Error);
  const auto Address = (**Mapped).Value,
             Allocated = Space->physicalMemory()->allocatedBytes();
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  error(ServiceKind::Fcntl, {A, Command}, 9);
  EXPECT_EQ(complete(D), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  error(ServiceKind::Fcntl, {D, Command}, 9);
  std::array<uint8_t, 6> Bytes;
  ASSERT_FALSE(bool(Space->read(Address, Bytes)));
  EXPECT_EQ(Bytes, (std::array<uint8_t, 6>{'a', 'b', 0, 255, 'e', 'f'}));
  EXPECT_EQ(Space->physicalMemory()->allocatedBytes(), Allocated);
  EXPECT_GT(std::get<DarwinFiles::Mapping>(Snapshot).Lease.use_count(), 1);
  auto Unmapped = VM.handle(ServiceKind::Munmap, {0, 0, {Address, Page}, {}},
                            *Files, Result);
  ASSERT_TRUE(bool(Unmapped));
  ASSERT_TRUE(*Unmapped);
  EXPECT_FALSE((**Unmapped).Error);
}
TEST_P(DarwinFullSynchronizationTest,
       DirtyBytesTruncateAttributesAndLocksStayIndependent) {
  mutationPolicies();
  Options->ModeMutations.emplace().Objects["/data"] = {11, 12};
  Options->OwnershipMutations.emplace().Objects["/data"] = {13, 14};
  Options->AdvisoryLocks.emplace().Objects.insert("/data");
  reset();
  const auto A = open("/data", 2), B = open(), D = ok(ServiceKind::Dup, {A});
  const std::array<uint8_t, 2> Bytes{'X', 'Y'};
  llvm::cantFail(Space->write(Base + Page * 2, Bytes));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 2}), 2u);
  const auto Written = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  EXPECT_EQ(complete(D), 0u);
  EXPECT_EQ(status(A), Written);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), Flags);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 3}), 0u);
  const auto Truncated = status(A);
  EXPECT_EQ(complete(A), 0u);
  EXPECT_EQ(status(D), Truncated);
  std::array<uint8_t, 3> Read;
  EXPECT_EQ(ok(ServiceKind::Pread, {D, Base + Page * 2, 3, 0}), 3u);
  llvm::cantFail(Space->read(Base + Page * 2, Read));
  EXPECT_EQ(Read, (std::array<uint8_t, 3>{'X', 'Y', 0}));
  EXPECT_EQ(ok(ServiceKind::Fchmod, {A, 0640}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchown, {A, UINT32_MAX, UINT32_MAX}), 0u);
  const auto Current = status(A);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 6}), 0u);
  EXPECT_EQ(complete(B), 0u);
  error(ServiceKind::Flock, {B, 6}, 35);
  EXPECT_EQ(status(A), Current);
  EXPECT_EQ(ok(ServiceKind::Flock, {A, 8}), 0u);
  EXPECT_EQ(complete(A), 0u);
  EXPECT_EQ(ok(ServiceKind::Flock, {B, 6}), 0u);
}
TEST_P(DarwinFullSynchronizationTest,
       CompletionNeverRestoresInvalidObservations) {
  Options->WritableFiles.insert("/data");
  Options->ExtendedAttributes["/data"] = {{"tag", {1, 2}}};
  reset();
  const auto A = open("/data", 2);
  const std::array<uint8_t, 1> Bytes{'X'};
  llvm::cantFail(Space->write(Base + Page * 2, Bytes));
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + Page * 2, 1}), 1u);
  refused(ServiceKind::Fstat64, {A, Base + Page + 8},
          diagnostic::FileMutatedMetadata);
  EXPECT_EQ(complete(A), 0u);
  refused(ServiceKind::Fstat64, {A, Base + Page + 8},
          diagnostic::FileMutatedMetadata);
  const auto Name = path("tag", Base + 128);
  EXPECT_FALSE(
      invoke(ServiceKind::FgetXattr, {A, Name, Base + Page * 2, 2, 0, 0}));
  const auto Unknown = Result.Diagnostic;
  EXPECT_EQ(complete(A), 0u);
  EXPECT_FALSE(
      invoke(ServiceKind::FgetXattr, {A, Name, Base + Page * 2, 2, 0, 0}));
  EXPECT_EQ(Result.Diagnostic, Unknown);
}
TEST_P(DarwinFullSynchronizationTest,
       IndependentNewObjectFlagsAndInitialNames) {
  mutationPolicies();
  objects().clear();
  Options->FullSynchronization->CreatedRegularObjects = Command == 51;
  Options->BarrierSynchronization->CreatedRegularObjects = Command == 85;
  reset();
  refusedCompletion(open(), provider());
  refusedCompletion(open("/data", 0x200), provider());
  const auto A = open("/fresh", 0x202);
  const auto Before = status(A);
  EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 16), 100u);
  EXPECT_EQ(llvm::support::endian::read32le(Before.data() + 24), 501u);
  EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 72), 41u);
  EXPECT_EQ(complete(A), 0u);
  EXPECT_EQ(status(A), Before);
  const auto Other = Command == 51 ? 85u : 51u;
  refused(ServiceKind::Fcntl, {A, Other},
          Command == 51 ? diagnostic::FileBarrierSynchronizationProvider
                        : diagnostic::FileFullSynchronizationProvider);
  EXPECT_EQ(ok(ServiceKind::Link, {path("/fresh"), path("/alias", Base + 128)}),
            0u);
  EXPECT_EQ(
      ok(ServiceKind::Rename, {path("/fresh"), path("/moved", Base + 128)}),
      0u);
  const auto Alias = open("/alias");
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/alias")}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {path("/moved")}), 0u);
  EXPECT_EQ(complete(Alias), 0u);
  const auto Held = status(A);
  EXPECT_EQ(llvm::support::endian::read16le(Held.data() + 14), 0u);
  EXPECT_EQ(complete(A), 0u);
  const auto New = open("/fresh", 0x202);
  EXPECT_EQ(complete(New), 0u);
  EXPECT_NE(llvm::support::endian::read64le(status(New).data() + 16), 100u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {path("/directory"), 0700}), 0u);
  refusedCompletion(open("/directory"), target());
  EXPECT_EQ(
      ok(ServiceKind::Symlink, {path("fresh"), path("/link", Base + 128)}), 0u);
  refusedCompletion(open("/link", 0x200000), target());
}
TEST_P(DarwinFullSynchronizationTest,
       FailedCreationDoesNotConsumeGrantOrInode) {
  mutationPolicies();
  Options->FullSynchronization->CreatedRegularObjects = true;
  Options->BarrierSynchronization->CreatedRegularObjects = true;
  Options->DescriptorLimit = 4;
  reset();
  const auto A = open();
  error(ServiceKind::Open, {path("/failed"), 0x202, 0600}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  const auto New = open("/failed", 0x202);
  EXPECT_EQ(llvm::support::endian::read64le(status(New).data() + 16), 100u);
  EXPECT_EQ(complete(New), 0u);
  Options->CreationPolicy->FirstInode = UINT64_MAX;
  reset();
  const auto Last = open("/last", 0x202);
  EXPECT_EQ(complete(Last), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Last}), 0u);
  refused(ServiceKind::Open, {path("/exhausted"), 0x202, 0600},
          diagnostic::FileCreationInode);
  const auto D = open("/last");
  EXPECT_EQ(complete(D), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(status(D).data() + 16), UINT64_MAX);
}
INSTANTIATE_TEST_SUITE_P(
    PagesAndCommands, DarwinFullSynchronizationTest,
    testing::Combine(testing::Values(uint64_t(4096), uint64_t(16384)),
                     testing::Values(uint32_t(51), uint32_t(85))));
} // namespace
} // namespace neverd::emulation::darwin_model
