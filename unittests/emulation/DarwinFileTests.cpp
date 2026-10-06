//===- DarwinFileTests.cpp - Darwin descriptor and copy contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
TEST(DarwinFileOptions, AdmissionCountsPathsTerminatorsFilesAndInputTogether) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 4);
  O.StandardInput = {0xff};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/b"] = {};
  auto TooLarge = validateFileOptions(O);
  ASSERT_TRUE(bool(TooLarge));
  llvm::consumeError(std::move(TooLarge));
  O = {};
  for (unsigned I = 0; I != 256; ++I)
    O.Files["/file" + std::to_string(I)] = {};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/extra"] = {};
  auto TooMany = validateFileOptions(O);
  ASSERT_TRUE(bool(TooMany));
  llvm::consumeError(std::move(TooMany));
  O = {};
  O.Files['/' + std::string(256, 'a')] = {};
  auto LongName = validateFileOptions(O);
  ASSERT_TRUE(bool(LongName));
  llvm::consumeError(std::move(LongName));
}
TEST(DarwinFileOptions, SwapDeclarationRequiresExplicitMutableDirectory) {
  DarwinFileOptions O;
  O.SwapRenameDirectories.insert("/");
  auto Refused = [&](const DarwinFileOptions &Input) {
    auto E = validateFileOptions(Input);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  Refused(O);
  O.MutableDirectories.insert("/");
  Refused(O);
  O.Directories.insert("/");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  for (const char *Path : {"/missing", "/file", "/implicit"}) {
    auto Bad = O;
    Bad.Files["/file"] = {};
    Bad.Files["/implicit/child"] = {};
    Bad.MutableDirectories.insert(Path);
    Bad.SwapRenameDirectories.insert(Path);
    Refused(Bad);
  }
}
TEST(DarwinFileOptions, SwapDeclarationChargesAReferenceWithoutAnotherEntry) {
  DarwinFileOptions O;
  O.Directories.insert("/");
  O.MutableDirectories.insert("/");
  O.SwapRenameDirectories.insert("/");
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 9);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  auto TooLarge = validateFileOptions(O);
  EXPECT_TRUE(bool(TooLarge));
  llvm::consumeError(std::move(TooLarge));
  O.Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    O.Files["/f" + std::to_string(I)] = {};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/extra"] = {};
  auto TooMany = validateFileOptions(O);
  EXPECT_TRUE(bool(TooMany));
  llvm::consumeError(std::move(TooMany));
}
class DarwinFileTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  uint64_t Page;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    Options.emplace();
    Options->Files["/data"] = {'a', 'b', 0, 0xff, 'e', 'f'};
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
  }
  void path(llvm::StringRef Text, uint64_t Address = Base) {
    std::string Data = Text.str() + '\0';
    ASSERT_FALSE(bool(Space->write(
        Address,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Data.data()),
                                Data.size()))));
  }
  std::optional<ServiceResult> invoke(ServiceKind Kind,
                                      std::array<uint64_t, 6> Args) {
    auto Out = Files->handle(Kind, {0, 0, Args, std::nullopt}, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  uint64_t ok(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    auto Out = invoke(Kind, Args);
    EXPECT_TRUE(Out.has_value()) << Result.Diagnostic;
    if (!Out)
      return UINT64_MAX;
    EXPECT_FALSE(Out->Error) << Out->Value;
    return Out->Value;
  }
  void error(ServiceKind Kind, std::array<uint64_t, 6> Args, uint64_t Code) {
    auto Out = invoke(Kind, Args);
    ASSERT_TRUE(Out.has_value()) << Result.Diagnostic;
    EXPECT_TRUE(Out->Error);
    EXPECT_EQ(Out->Value, Code);
  }
  void contents(uint64_t FD, llvm::ArrayRef<uint8_t> Expected) {
    EXPECT_EQ(ok(ServiceKind::Pread, {FD, Base + Page, 64, 0}),
              Expected.size());
    std::vector<uint8_t> Bytes(Expected.size());
    ASSERT_FALSE(bool(Space->read(Base + Page, Bytes)));
    EXPECT_EQ(Bytes, Expected.vec());
  }
  void mutationPolicy(uint32_t Unit = 4096) {
    Options->WritableFiles.insert("/data");
    Options->Metadata["/data"] = darwin_test::mutationMetadata(6);
    Options->Metadata["/data"].Blocks = Unit / 512;
    Options->MutationPolicies["/data"] = {Unit, {-7, 123456789}};
  }
  void creationPolicy(uint64_t First = darwin_test::CreationPolicy.FirstInode) {
    Options->MutableDirectories.insert("/");
    Options->Metadata["/"] = darwin_test::creationParentMetadata();
    Options->InitialUmask = 0027;
    Options->CreationPolicy = darwin_test::CreationPolicy;
    Options->CreationPolicy->FirstInode = First;
  }
  std::array<uint8_t, 144> status(uint64_t FD) {
    EXPECT_EQ(ok(ServiceKind::Fstat64, {FD, Base + 256}), 0u);
    std::array<uint8_t, 144> Bytes;
    llvm::cantFail(Space->read(Base + 256, Bytes));
    return Bytes;
  }
  void identity(uint64_t FD, llvm::StringRef Expected) {
    std::vector<uint8_t> Bytes(Expected.size() + 2, 0xcc);
    llvm::cantFail(Space->write(Base + Page, Bytes));
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 50, Base + Page}), 0u);
    llvm::cantFail(Space->read(Base + Page, Bytes));
    EXPECT_EQ(llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                              Expected.size()),
              Expected);
    EXPECT_EQ(Bytes[Expected.size()], 0u);
    EXPECT_EQ(Bytes.back(), 0xccu);
  }
  void renameFile(llvm::StringRef Source, llvm::StringRef Target) {
    path(Source);
    path(Target, Base + 128);
    EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  }
  uint64_t makeDirectory(llvm::StringRef Name) {
    path(Name);
    EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0700}), 0u);
    return ok(ServiceKind::Open, {Base});
  }
  void swapFiles(llvm::StringRef Source, llvm::StringRef Target) {
    path(Source);
    path(Target, Base + 128);
    EXPECT_EQ(ok(ServiceKind::RenameAtX,
                 {999, Base, 999, Base + 128, 0x1234567800000012ULL}),
              0u);
  }
};

TEST_P(DarwinFileTest, SwapRetainsBothObjectsDescriptionsMetadataAndMappings) {
  mutationPolicy();
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/target"] = {'T', 'G'};
  Options->WritableFiles.insert("/target");
  auto M = darwin_test::mutationMetadata(2);
  M.Inode++;
  M.GID = 77;
  M.Blocks = 1;
  Options->Metadata["/target"] = M;
  Options->MutationPolicies["/target"] = {512, {9, 42}};
  const auto A = ok(ServiceKind::Open, {Base, 0x100000a});
  const auto Independent = ok(ServiceKind::Open, {Base});
  const auto Duplicate = ok(ServiceKind::Dup, {A});
  auto Source = status(A);
  path("/target");
  const auto B = ok(ServiceKind::Open, {Base, 2});
  auto Target = status(B);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4, 0}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 1, 0}), 1u);
  auto SourceLease = Files->mappingSource(A);
  auto TargetLease = Files->mappingSource(B);
  swapFiles("/data", "/target");
  for (auto FD : {A, Independent, Duplicate})
    identity(FD, "/target");
  identity(B, "/data");
  llvm::support::endian::write64le(Source.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Source.data() + 72, 123456789);
  llvm::support::endian::write64le(Target.data() + 64, 9);
  llvm::support::endian::write64le(Target.data() + 72, 42);
  EXPECT_EQ(status(A), Source);
  EXPECT_EQ(status(B), Target);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Duplicate, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(SourceLease).Bytes[0], 'a');
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes[0], 'T');
  path("/data");
  const auto Reopened = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(status(Reopened), Target);
  contents(Reopened, {'T', 'G'});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  SourceLease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 1}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  TargetLease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 0}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(A).data() + 6), 1u);
  EXPECT_EQ(llvm::support::endian::read16le(status(B).data() + 6), 1u);
  EXPECT_EQ(Options->Metadata.at("/target").Inode, M.Inode);
  EXPECT_EQ(Options->Metadata.at("/target").GID, M.GID);
  EXPECT_EQ(Options->Metadata.at("/target").ChangeTime.Seconds,
            M.ChangeTime.Seconds);
  EXPECT_EQ(Options->Files.at("/target"), (std::vector<uint8_t>{'T', 'G'}));
}

TEST_P(DarwinFileTest, SwapRetainsOwnGrantsAcrossCreatedParents) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  auto Original = darwin_test::mutationMetadata(6);
  Original.GID = 7;
  Options->Metadata["/data"] = Original;
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Child = makeDirectory("/child");
  path("/child/target");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  auto Before = status(B);
  swapFiles("/data", "/child/target");
  identity(A, "/child/target");
  identity(B, "/data");
  llvm::support::endian::write64le(Before.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Before.data() + 72, 123456789);
  EXPECT_EQ(status(B), Before);
  error(ServiceKind::Ftruncate, {A, 0}, 22);
  path("/child/target");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 1}), 0u);
  path("target");
  const auto Reopened = ok(ServiceKind::OpenAt, {Child, Base});
  contents(Reopened, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(Options->Metadata.at("/data").Inode, Original.Inode);
  EXPECT_EQ(Options->Metadata.at("/data").GID, Original.GID);
  EXPECT_EQ(Options->Metadata.at("/data").ChangeTime.Seconds,
            Original.ChangeTime.Seconds);
}

TEST_P(DarwinFileTest, SwapDeclarationFollowsOriginalDirectoryObjects) {
  for (bool RootDeclared : {false, true}) {
    Options.emplace();
    Options->Files["/data"] = {'d'};
    Options->Directories = {"/", "/empty"};
    Options->MutableDirectories = {"/", "/empty"};
    Options->RemovableDirectories.insert("/empty");
    Options->SwapRenameDirectories.insert("/empty");
    if (RootDeclared)
      Options->SwapRenameDirectories.insert("/");
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
    const auto A = ok(ServiceKind::Open, {Base});
    path("/empty");
    const auto Old = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
    const auto New = makeDirectory("/empty");
    path("/empty/target");
    const auto B = ok(ServiceKind::Open, {Base, 0x202});
    path("/data");
    path("/empty/target", Base + 128);
    if (RootDeclared) {
      EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, New, Base + 128, 2}),
                0u);
      identity(A, "/empty/target");
      identity(B, "/data");
    } else {
      EXPECT_FALSE(
          invoke(ServiceKind::RenameAtX, {999, Base, New, Base + 128, 2}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
      identity(A, "/data");
      identity(B, "/empty/target");
    }
    identity(Old, "/empty");
    EXPECT_EQ(Options->SwapRenameDirectories.contains("/"), RootDeclared);
  }
}

TEST_P(DarwinFileTest, SwapKeepsLookupAndFlagErrorsBeforeDeclaration) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  Options->Directories.insert("/folder");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  for (uint64_t Flags : {2u, 0x12u}) {
    path("/data");
    for (auto [Name, Code] : {std::pair{"/missing", 2u},
                              {"/missing/", 2u},
                              {"/target/", 20u},
                              {"/folder/.", 22u},
                              {"/folder/..", 22u},
                              {"/missing/../target", 2u}}) {
      path(Name, Base + 128);
      error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}, Code);
    }
    error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, Flags}, 14);
    path("/missing");
    error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, Flags}, 2);
    path("/data");
    path("/folder", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapKind);
    path("/target", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
  }
  for (uint64_t Flags : {6u, 8u, 0x16u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          22);
  for (uint64_t Flags : {1u, 3u, 0x13u}) {
    EXPECT_FALSE(invoke(ServiceKind::RenameAtX,
                        {999, UINT64_MAX, 999, UINT64_MAX, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameFlags);
  }
  EXPECT_EQ(status(A), Before);
  identity(A, "/data");
}

TEST_P(DarwinFileTest, SwapRequiresSharedDomainAuthorityAndConsistentDevices) {
  Options->Directories = {"/", "/other"};
  Options->MutableDirectories = {"/", "/other"};
  Options->SwapRenameDirectories = {"/", "/other"};
  Options->Files["/other/target"] = {'T'};
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Metadata["/other"] = darwin_test::creationParentMetadata();
  Options->Metadata["/other"].Inode++;
  const auto A = ok(ServiceKind::Open, {Base});
  path("/other/target", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(A, "/data");
  Options->Files.erase("/other/target");
  Options->Files["/target"] = {'T'};
  Options->Metadata["/target"] = darwin_test::mutationMetadata(1);
  Options->Metadata["/target"].Device++;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/data");
  path("/target", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  Options->MutableDirectories.clear();
  Options->SwapRenameDirectories.clear();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
}

TEST_P(DarwinFileTest, SwapNoopPreservesObservationsWithoutCapabilityOrCharge) {
  mutationPolicy();
  creationPolicy();
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  const auto Lease = Files->mappingSource(A);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 3, 0}), 3u);
  swapFiles("/data", "/./data");
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(Parent), ParentBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  path("/fresh");
  const auto B = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(llvm::support::endian::read64le(status(B).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_TRUE(Options->SwapRenameDirectories.empty());
}

TEST_P(DarwinFileTest, SwapConsumesNoDescriptorEntryOrCreationInode) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  swapFiles("/data", "/target");
  path("/fresh");
  const auto A = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  Options->CreationPolicy.reset();
  Options->InitialUmask.reset();
  Options->Metadata.clear();
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  swapFiles("/f0", "/f1");
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  Options->DescriptorLimit = 3;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  swapFiles("/f0", "/f1");
  path("/f0");
  error(ServiceKind::Open, {Base}, 24);
}

TEST_P(DarwinFileTest,
       SwapChargesBothNamesWithoutCreditingLinkedBytesOrLeases) {
  for (unsigned Retained = 0; Retained != 4; ++Retained) {
    SCOPED_TRACE(Retained);
    Options.emplace();
    Options->Files["/data"] = std::vector<uint8_t>(6, 'd');
    Options->Files["/target"] =
        std::vector<uint8_t>(darwin_file_limits::Bytes - 45, 't');
    Options->WritableFiles.insert("/data");
    Options->Directories.insert("/");
    Options->MutableDirectories.insert("/");
    Options->SwapRenameDirectories.insert("/");
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
    const auto A = ok(ServiceKind::Open, {Base, 2});
    path("/target");
    const auto B = ok(ServiceKind::Open, {Base});
    auto Lease = Retained & 2 ? Files->mappingSource(B)
                              : DarwinFiles::MappingSource(uint32_t(0));
    if (!(Retained & 1))
      EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
    path("/data");
    path("/target", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(A, "/data");
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 5}), 0u);
    swapFiles("/data", "/target");
    identity(A, "/target");
    if (Retained & 1)
      identity(B, "/data");
    if (Retained & 2)
      EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.size(),
                darwin_file_limits::Bytes - 45);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 6}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    Lease = uint32_t(0);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 6}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    swapFiles("/target", "/data");
    swapFiles("/data", "/target");
    identity(A, "/target");
    contents(A, std::vector<uint8_t>(5, 'd'));
  }
}

TEST_P(DarwinFileTest, DirectoryCreationDistinguishesTrailingSlashesFromDots) {
  Options->MutableDirectories.insert("/");
  for (const char *Name : {"/data/", "/data/.", "/data/.."}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base, 0700}, 20);
  }
  for (const char *Name : {"/absent/.", "/absent/..", "/absent/child"}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base, 0700}, 2);
  }
  for (const char *Name : {"/", "/data", "/."}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base, UINT64_MAX}, 17);
  }
  path("/new///");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0x12345678ffff01c0ULL}), 0u);
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  for (const char *Name : {"/new", "/new/.", "/new/.."}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base}, 17);
  }
  path("/new/child//");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  error(ServiceKind::Open, {Base, 2}, 21);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  path("/new/.");
  error(ServiceKind::Rmdir, {Base}, 22);
  path("/new///");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_TRUE(Options->Directories.empty());
}

TEST_P(DarwinFileTest, DirectoryAtCallsKeepFlagsCarrierAndLookupOrder) {
  Options->MutableDirectories.insert("/");
  const auto Regular = ok(ServiceKind::Open, {Base});
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  error(ServiceKind::MkdirAt, {UINT64_MAX, 1, 0700}, 14);
  error(ServiceKind::MkdirAt, {UINT64_MAX, Base, 0700}, 9);
  error(ServiceKind::MkdirAt, {Regular, Base, 0700}, 20);
  EXPECT_FALSE(invoke(ServiceKind::MkdirAt, {1, Base, 0700}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_EQ(ok(ServiceKind::MkdirAt,
               {0x1234567800000000ULL | Root, Base, UINT64_MAX}),
            0u);
  error(ServiceKind::UnlinkAt, {UINT64_MAX, 1, 0x40}, 22);
  error(ServiceKind::UnlinkAt, {UINT64_MAX, 1, 0x880}, 14);
  for (uint32_t Flags : {0x100u, 0x180u, 0x1000u, 0x1880u}) {
    EXPECT_FALSE(invoke(ServiceKind::UnlinkAt, {UINT64_MAX, 1, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::UnlinkFlags);
  }
  error(ServiceKind::UnlinkAt, {Regular, Base, 0x80}, 20);
  path("");
  error(ServiceKind::MkdirAt, {UINT64_MAX, Base}, 9);
  error(ServiceKind::MkdirAt, {Regular, Base}, 20);
  error(ServiceKind::MkdirAt, {Root, Base}, 2);
  path("/new");
  EXPECT_EQ(
      ok(ServiceKind::UnlinkAt, {UINT64_MAX, Base, 0xfedcba9800000880ULL}), 0u);
  EXPECT_EQ(ok(ServiceKind::MkdirAt, {UINT64_MAX, Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  Options.reset();
  error(ServiceKind::UnlinkAt, {UINT64_MAX, 1, 0x40}, 22);
  EXPECT_FALSE(invoke(ServiceKind::MkdirAt, {UINT64_MAX, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
}

TEST_P(DarwinFileTest, DirectoryCreationRequiresGrantsButNoFreeDescriptor) {
  Options->Directories.insert("/fixed");
  Options->DescriptorLimit = 3;
  path("/fixed/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/fixed");
  error(ServiceKind::Mkdir, {Base}, 17);
  EXPECT_FALSE(invoke(ServiceKind::Rmdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryRemovalInitial);
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  error(ServiceKind::Open, {Base}, 24);
  path("/new/child");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/data");
  error(ServiceKind::Rmdir, {Base}, 20);
  path("/");
  error(ServiceKind::Rmdir, {Base}, 21);
}

TEST_P(DarwinFileTest, RootRemovalDistinguishesSlashOnlyFromDotComponents) {
  for (const char *Name : {"/", "//", "///", "/.", "/..", "//.//"}) {
    SCOPED_TRACE(Name);
    path(Name);
    const uint64_t Code = llvm::StringRef(Name).contains('.') ? 16 : 21;
    error(ServiceKind::Rmdir, {Base}, Code);
    error(ServiceKind::Unlink, {Base}, Code);
    error(ServiceKind::UnlinkAt, {UINT64_MAX, Base, 0}, Code);
    error(ServiceKind::UnlinkAt, {UINT64_MAX, Base, 0x80}, Code);
    EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  }
}

TEST_P(DarwinFileTest, DirectoryRemovalPreservesHeldAndNonemptyObjects) {
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Directory});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
  path("child");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/new/child/..");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/new/child");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  identity(Dup, "/new");
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Dup}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Dup}), 0u);
  path(".");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("/new");
  const auto File = ok(ServiceKind::Open, {Base, 0x202});
  error(ServiceKind::Open, {Base, 0x100000}, 20);
  identity(File, "/new");
  path(".");
  const auto OldDirectory = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {OldDirectory, UINT64_MAX, 0}, 21);
  path("/", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base + 128}), 0u);
}

TEST(DarwinFileOptions, RemovableDirectoriesRequireExplicitConsistentIdentity) {
  DarwinFileOptions Good;
  Good.Files["/data"] = {};
  Good.Directories.insert("/empty");
  Good.MutableDirectories.insert("/");
  Good.RemovableDirectories.insert("/empty");
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (const char *Name : {"/", "/missing", "/data"}) {
    auto O = Good;
    O.RemovableDirectories = {Name};
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryRemovableOption);
  }
  auto O = Good;
  O.MutableDirectories.clear();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryRemovableOption);
  O = Good;
  O.Files["/implicit/child"] = {};
  O.RemovableDirectories = {"/implicit"};
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryRemovableOption);
  auto M = darwin_test::mutationMetadata(64);
  M.Mode = 0040755;
  M.Inode = 41;
  Good.Metadata["/"] = M;
  M.Inode = 42;
  Good.Metadata["/empty"] = M;
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (unsigned Flag : {1u, 2u, 4u, 0x20000u, 0x40000u, 0x100000u}) {
    O = Good;
    O.Metadata["/empty"].Flags = Flag;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceFlags);
  }
  for (unsigned Bits : {01000u, 02000u, 04000u}) {
    O = Good;
    O.Metadata["/empty"].Mode |= Bits;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceFlags);
  }
  O = Good;
  O.Metadata["/empty"].Device++;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryRemovalDevice);
  O = Good;
  O.Directories.insert("/alias");
  O.Metadata["/alias"] = O.Metadata.at("/empty");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)), diagnostic::NamespaceAlias);
  O.Metadata["/alias"].Device++;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  // Snapshot identities also constrain removal without complete stat inputs.
  O = {};
  O.Directories = {"/empty", "/alias"};
  O.MutableDirectories.insert("/");
  O.RemovableDirectories.insert("/empty");
  O.DirectoryContents["/"] = darwin_test::directoryContents();
  auto &Alias = O.DirectoryContents["/"].Entries[3];
  Alias.Name = "alias";
  Alias.Type = 4;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  Alias.Inode = O.DirectoryContents["/"].Entries[2].Inode;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)), diagnostic::NamespaceAlias);
}

TEST(DarwinFileOptions, RemovableReferencesJoinTheInitialStorageFootprint) {
  DarwinFileOptions O;
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 18);
  O.Directories.insert("/old");
  O.MutableDirectories.insert("/");
  O.RemovableDirectories.insert("/old");
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.StandardInput = {0};
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, InitialDirectoryRemovalKeepsCWDAndReusedFileSeparate) {
  Options->Directories.insert("/empty");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/empty");
  Options->WorkingDirectory = "/empty";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/empty");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Old});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  const auto New = ok(ServiceKind::Open, {Base, 0x202});
  path("xy", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {New, Base + 128, 2}), 2u);
  identity(Old, "/empty");
  identity(New, "/empty");
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Dup}), 0u);
  path(".");
  const auto CWD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {CWD, UINT64_MAX, 0}, 21);
  path("child");
  error(ServiceKind::Access, {Base}, 2);
  path("../empty");
  const auto SameNew = ok(ServiceKind::Open, {Base});
  contents(SameNew, {'x', 'y'});
  EXPECT_TRUE(Options->Directories.contains("/empty"));
  EXPECT_EQ(Options->WorkingDirectory, "/empty");
  EXPECT_TRUE(Options->RemovableDirectories.contains("/empty"));
}

TEST_P(DarwinFileTest, InitialDirectorySnapshotsNeverReturnAfterNameReuse) {
  Options->Directories.insert("/empty");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/empty");
  auto M = darwin_test::mutationMetadata(64);
  M.Mode = 0040755;
  M.Inode = 41;
  Options->Metadata["/"] = M;
  M.Inode = 0xfedcba9876543211ULL;
  Options->Metadata["/empty"] = M;
  auto RootSnapshot = darwin_test::directoryContents();
  RootSnapshot.Entries[2].Inode = M.Inode;
  Options->DirectoryContents["/"] = RootSnapshot;
  auto Snapshot = darwin_test::directoryContents();
  Snapshot.Entries.resize(2);
  Snapshot.Entries[0].Inode = M.Inode;
  Options->DirectoryContents["/empty"] = Snapshot;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("/empty");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Before = status(Old);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Old, Base + Page, 64, Base + 512}),
      64u);
  const auto Cursor = ok(ServiceKind::Lseek, {Old, 0, 1});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Old, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {Old, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Old, 0, 1}), Cursor);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto New = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {New, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {New, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  EXPECT_EQ(ok(ServiceKind::Lseek, {New, 0, 1}), 0u);
  EXPECT_EQ(Options->Metadata.at("/empty").Inode,
            llvm::support::endian::read64le(Before.data() + 8));
  EXPECT_EQ(Options->DirectoryContents.at("/empty").Entries.size(), 2u);
}

TEST_P(DarwinFileTest, InitialDirectoryParentChainDoesNotAttachToReplacement) {
  Options->Directories = {"/p", "/p/c"};
  Options->MutableDirectories = {"/", "/p"};
  Options->RemovableDirectories = {"/p", "/p/c"};
  Options->WorkingDirectory = "/p/c";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/p");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("../c");
  error(ServiceKind::Access, {Base}, 2);
  path("../../p/c");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("..");
  const auto Parent = ok(ServiceKind::Open, {Base});
  identity(Parent, "/p");
  path("c");
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  path("../.");
  error(ServiceKind::Mkdir, {Base}, 2);
  EXPECT_TRUE(Options->Directories.contains("/p/c"));
}

TEST_P(DarwinFileTest, InitialDirectoryImplicitChildrenSurviveRegularUnlink) {
  Options->Files["/p/implicit/leaf"] = {'v'};
  Options->Directories.insert("/p");
  Options->MutableDirectories = {"/", "/p", "/p/implicit"};
  Options->RemovableDirectories.insert("/p");
  path("/p/implicit/leaf");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/p");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/p/implicit");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Rmdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryRemovalInitial);
  contents(Old, {'v'});
  // A separate admitted configuration can explicitly remove that initial child.
  Options->Directories.insert("/p/implicit");
  Options->RemovableDirectories.insert("/p/implicit");
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/p/implicit/leaf");
  const auto Held = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/p/implicit");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  path("/p/implicit");
  error(ServiceKind::Access, {Base}, 2);
  contents(Held, {'v'});
}

TEST_P(DarwinFileTest, InitialDirectoryTombstonesCountOnlyLiveReplacements) {
  Options->Directories = {"/p", "/p/c"};
  Options->MutableDirectories = {"/", "/p"};
  Options->RemovableDirectories = {"/p", "/p/c"};
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto File = ok(ServiceKind::Open, {Base, 0x202});
  path("/p");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  error(ServiceKind::Access, {Base}, 2);
  identity(File, "/p/c");
  contents(File, {});
}

TEST_P(DarwinFileTest, InitialDirectoryRemovalNeverRefundsFixedStorage) {
  Options->WritableFiles.insert("/data");
  Options->Directories.insert("/old");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/old");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 24;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  path("/old");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, InitialDirectoryRemovalNeverRefundsFixedEntries) {
  Options->Files.clear();
  for (unsigned I = 0; I != 254; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->Directories.insert("/old");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/old");
  path("/old");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  path("/f0");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/another");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
}

TEST_P(DarwinFileTest, InitialDirectoryRecreationInheritsTheCurrentParent) {
  creationPolicy();
  Options->Directories.insert("/empty");
  Options->RemovableDirectories.insert("/empty");
  auto Metadata = darwin_test::creationParentMetadata();
  Metadata.Inode++;
  Metadata.GID = 77;
  Options->Metadata["/empty"] = Metadata;
  path("/empty");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/empty/child");
  const auto Child = ok(ServiceKind::Open, {Base, 0x202});
  const auto Record = status(Child);
  EXPECT_EQ(llvm::support::endian::read32le(Record.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read32le(Record.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read64le(Record.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Old, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  path("child");
  error(ServiceKind::OpenAt, {Old, Base}, 2);
  EXPECT_EQ(Options->Metadata.at("/empty").GID, 77u);
}

TEST_P(DarwinFileTest, DeletedDirectoryDotOpensKeepIndependentCursors) {
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Old});
  path(".", Base + 128);
  const auto Independent = ok(ServiceKind::OpenAt, {Old, Base + 128});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Old, 7, 0}), 7u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto New = ok(ServiceKind::Open, {Base});
  path("/new/child");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("child");
  error(ServiceKind::OpenAt, {Old, Base}, 2);
  error(ServiceKind::FaccessAt, {Dup, Base}, 2);
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {New, Base}), 0u);
  path(".");
  const auto Reopened = ok(ServiceKind::OpenAt, {Old, Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Reopened, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {New, 0, 1}), 0u);
  identity(Reopened, "/new");
  error(ServiceKind::Read, {Old, UINT64_MAX, 0}, 21);
  EXPECT_EQ(std::get<uint32_t>(Files->mappingSource(Old)), 22u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Old, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {Old, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {New, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
}

TEST_P(DarwinFileTest, DeletedDirectoryParentsRemainObjectsAfterNameReuse) {
  Options->MutableDirectories.insert("/");
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Child = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c/fresh");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("..");
  const auto Parent = ok(ServiceKind::Open, {Base});
  identity(Parent, "/p");
  path("c");
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  path("../../p/c/fresh");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("../c");
  error(ServiceKind::Access, {Base}, 2);
  path("..");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Parent}), 0u);
  path("c");
  error(ServiceKind::Access, {Base}, 2);
  path("../p/c");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  path("fresh");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
}

TEST_P(DarwinFileTest, DeletedDirectoryNamespaceLookupKeepsNativeIntent) {
  Options->MutableDirectories.insert("/");
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Child = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("..");
  error(ServiceKind::MkdirAt, {Child, Base}, 17);
  error(ServiceKind::OpenAt, {Child, Base, 0xa00}, 17);
  error(ServiceKind::UnlinkAt, {Child, Base, 0}, 1);
  error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 2);
  path("../.");
  error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 22);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  for (const char *Name : {"..", "../.", "./..", "../..", "../../."}) {
    SCOPED_TRACE(Name);
    path(Name);
    EXPECT_EQ(ok(ServiceKind::FaccessAt, {Child, Base}), 0u);
    error(ServiceKind::MkdirAt, {Child, Base}, 2);
    error(ServiceKind::OpenAt, {Child, Base, 0xa00}, 2);
    error(ServiceKind::UnlinkAt, {Child, Base, 0}, 2);
    error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 2);
  }
  for (const char *Name : {".", "./", "./."}) {
    path(Name);
    error(ServiceKind::MkdirAt, {Child, Base}, 17);
    error(ServiceKind::OpenAt, {Child, Base, 0xa00}, 17);
    error(ServiceKind::UnlinkAt, {Child, Base, 0}, 1);
    error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 22);
  }
  path("new");
  error(ServiceKind::MkdirAt, {Child, Base}, 2);
  error(ServiceKind::OpenAt, {Child, Base, 0x202}, 2);
  error(ServiceKind::FaccessAt, {Child, Base, 4}, 2);
  error(ServiceKind::MkdirAt, {UINT64_MAX, 1}, 14);
  error(ServiceKind::UnlinkAt, {Child, 1, 0x80}, 14);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {Child, Base}), 0u);
  error(ServiceKind::MkdirAt, {Child, Base}, 17);
  path("");
  error(ServiceKind::OpenAt, {Child, Base}, 2);
  path("/data", Base + 128);
  for (const char *Name : {".", "./.", "..", "./..", "..//"}) {
    path(Name);
    error(ServiceKind::RenameAt, {UINT64_MAX, Base + 128, Child, Base}, 22);
  }
  for (const char *Name :
       {"../.", "../..", "../../.", "missing/..", "../../new"}) {
    path(Name);
    error(ServiceKind::RenameAt, {UINT64_MAX, Base + 128, Child, Base}, 2);
  }
  path("../../data");
  error(ServiceKind::RenameAt, {Child, Base, UINT64_MAX, Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
}

TEST_P(DarwinFileTest, DeletedDirectoryPathBudgetRetainsCWDThenReclaims) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/x");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  error(ServiceKind::Access, {Base}, 2);
  path(".");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("/");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, DeletedDirectoryEntryBudgetRetainsParentChains) {
  Options->Files.clear();
  for (unsigned I = 0; I != 253; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Child = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/a");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  path("/", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/b");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/c");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
}

TEST_P(DarwinFileTest, DeletedDirectoryDup2ReleasesOnlyReplacedObjects) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Directory});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity}));
  identity(Dup, "/new");
  EXPECT_EQ(ok(ServiceKind::Dup2, {Data, Dup}), Dup);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  identity(Dup, "/data");
}

TEST_P(DarwinFileTest, DeletedDirectoryCannotAcquireReusedFileMetadata) {
  mutationPolicy();
  creationPolicy();
  const auto Original = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto Replacement = ok(ServiceKind::Open, {Base, 0x202, 0600});
  const auto NewStatus = status(Replacement);
  EXPECT_EQ(llvm::support::endian::read64le(NewStatus.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path(".");
  const auto Reopened = ok(ServiceKind::OpenAt, {Directory, Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Reopened, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  error(ServiceKind::Read, {Reopened, UINT64_MAX, 0}, 21);
  identity(Reopened, "/data");
  contents(Original, {'a', 'b', 0, 0xff, 'e', 'f'});
  contents(Replacement, {});
  EXPECT_EQ(status(Replacement), NewStatus);
}

TEST_P(DarwinFileTest, DirectoryPublicationInvalidatesOnlyChangedParents) {
  Options->WritableFiles.insert("/data");
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  auto RootMetadata = darwin_test::mutationMetadata(64);
  RootMetadata.Mode = 0040755;
  RootMetadata.Inode = 41;
  Options->Metadata["/"] = RootMetadata;
  Options->MutableDirectories.insert("/");
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  const auto Before = status(Root);
  path("/data");
  error(ServiceKind::Mkdir, {Base}, 17);
  path("/empty/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(status(Root), Before);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Root, Base + Page, 64, Base + 512}),
      64u);
  const auto Cursor = ok(ServiceKind::Lseek, {Root, 0, 1});
  path("/data", Base + 128);
  const auto Data = ok(ServiceKind::Open, {Base + 128, 2});
  // Initial paths/references cost 23 bytes; four observed records cost 128.
  // Four spare bytes cannot pay for "/new" including its terminator.
  EXPECT_EQ(
      ok(ServiceKind::Ftruncate, {Data, darwin_file_limits::Bytes - 151 - 4}),
      0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(status(Root), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), Cursor);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Root, Base + Page, 64, Base + 512}),
      64u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 512, 8)), Cursor);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, Cursor, 0}), Cursor);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {Root, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), Cursor);
  const auto New = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {New, UINT64_MAX}));
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {New, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  EXPECT_EQ(ok(ServiceKind::Close, {New}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(Options->Metadata.at("/").Size, 64u);
}

TEST_P(DarwinFileTest, DirectoryAndFileNamesShareEntryBudgetAndReclaim) {
  Options->Files.clear();
  for (unsigned I = 0; I != 254; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/extra", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base + 128, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/f0");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base + 128}));
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  for (unsigned I = 0; I != 300; ++I) {
    SCOPED_TRACE(I);
    path("/d" + std::to_string(I));
    EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  }
}

TEST_P(DarwinFileTest, DirectoryRefundDoesNotReleaseOrphanFileStorage) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/x", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base + 128}));
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 14}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new/f");
  const auto Child = ok(ServiceKind::Open, {Base, 0x202});
  path("xy", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {Child, Base + 128, 2}), 2u);
  auto Lease = Files->mappingSource(Child);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 9}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 8}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'x', 'y'}));
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, DirectoryReuseShadowsOldFileMetadataAndKeepsItsLease) {
  mutationPolicy();
  creationPolicy();
  Options->Metadata["/data"].Device = 456;
  Options->Metadata["/data"].GID = 123;
  const auto Old = ok(ServiceKind::Open, {Base});
  auto Lease = Files->mappingSource(Old);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto Unlinked = status(Old);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Directory, UINT64_MAX}));
  path("/data/child");
  const auto Child = ok(ServiceKind::Open, {Base, 0x202, 0777});
  const auto Record = status(Child);
  EXPECT_EQ(llvm::support::endian::read32le(Record.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read32le(Record.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read64le(Record.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  renameFile("/data/child", "/data/child");
  EXPECT_EQ(status(Child), Record);
  path("/data/moved");
  const auto Target = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("t", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {Target, Base + 128, 1}), 1u);
  renameFile("/data/child", "/data/moved");
  identity(Child, "/data/moved");
  contents(Target, {'t'});
  EXPECT_EQ(llvm::support::endian::read16le(status(Target).data() + 6), 0u);
  EXPECT_EQ(status(Old), Unlinked);
  contents(Old, Options->Files.at("/data"));
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Target}), 0u);
  path("/data/moved");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto New = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(New).data() + 8),
            darwin_test::CreationPolicy.FirstInode + 2);
  EXPECT_EQ(status(Old), Unlinked);
  identity(Old, "/data");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Options->Files.at("/data")));
  EXPECT_EQ(Options->Metadata.at("/data").GID, 123u);
}

TEST_P(DarwinFileTest, DirectoryIdentityInheritanceDoesNotInventDirectoryStat) {
  creationPolicy();
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0}), 0u);
  path("/new/deep");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, UINT64_MAX}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Directory, UINT64_MAX}));
  path("child");
  const auto File = ok(ServiceKind::OpenAt, {Directory, Base, 0x202, 0777});
  const auto Record = status(File);
  EXPECT_EQ(llvm::support::endian::read32le(Record.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read32le(Record.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read16le(Record.data() + 4), 0100750u);
  EXPECT_EQ(llvm::support::endian::read64le(Record.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path("/new/deep");
  error(ServiceKind::Rmdir, {Base}, 66);
  EXPECT_EQ(Options->CreationPolicy->FirstInode,
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest, DirectoryRemovalAndReuseRetainUnlinkedChildObjects) {
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new/f");
  const auto Old = ok(ServiceKind::Open, {Base, 0x202});
  path("old!", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {Old, Base + 128, 4}), 4u);
  auto Lease = Files->mappingSource(Old);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new/f");
  const auto New = ok(ServiceKind::Open, {Base, 0x202});
  path("new!", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {New, Base + 128, 4}), 4u);
  contents(Old, {'o', 'l', 'd', '!'});
  contents(New, {'n', 'e', 'w', '!'});
  identity(Old, "/new/f");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'o', 'l', 'd', '!'}));
}

TEST_P(DarwinFileTest, DirectoryCreationBoundsCanonicalNamesAndInputCopy) {
  std::string Parent;
  for (char C : {'a', 'b', 'c', 'd'})
    Parent += '/' + std::string(240, C);
  Options->Directories.insert(Parent);
  Options->MutableDirectories.insert(Parent);
  Options->WorkingDirectory = Parent;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path(std::string(59, 'x'));
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  path(std::string(58, 'y'));
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  identity(Directory, Parent + '/' + std::string(58, 'y'));
  path("z", Base + Page * 2 - 2);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base + Page * 2 - 2}), 0u);
  llvm::cantFail(Space->writeInteger(Base + Page * 2 - 1, 'q', 1));
  error(ServiceKind::Mkdir, {Base + Page * 2 - 1}, 14);
  path("q");
  error(ServiceKind::Access, {Base}, 2);
  llvm::cantFail(Space->protect(Base, Page * 2, Read | UserAccessible));
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
}

TEST_P(DarwinFileTest,
       AccessZeroActionBitsAndRequestedPermissionsStayDistinct) {
  const uint64_t Ignored[] = {
      0,        8,          0x80,       0x100,
      0x400000, 0x80000000, 0xffc001f8, 0x1234567800000000ULL};
  for (auto Mode : Ignored) {
    EXPECT_EQ(ok(ServiceKind::Access, {Base, Mode}), 0u);
    EXPECT_EQ(ok(ServiceKind::FaccessAt, {UINT64_MAX, Base, Mode, 0x830}), 0u);
  }
  const uint32_t Rights[] = {
      1,      2,      4,       0x200,   0x400,   0x800,   0x1000,   0x2000,
      0x4000, 0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000};
  for (auto Right : Rights) {
    for (uint64_t Mode : {uint64_t(Right), uint64_t(0xffc001f8) | Right}) {
      EXPECT_FALSE(invoke(ServiceKind::Access, {Base, Mode}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
      EXPECT_FALSE(invoke(ServiceKind::FaccessAt, {UINT64_MAX, Base, Mode}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
      error(ServiceKind::Access, {1, Mode}, 14);
      path("/missing", Base + 128);
      error(ServiceKind::Access, {Base + 128, Mode}, 2);
      error(ServiceKind::FaccessAt, {UINT64_MAX, Base + 128, Mode}, 2);
    }
  }
  EXPECT_FALSE(invoke(ServiceKind::Access, {Base, 0x80001000}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
}

TEST_P(DarwinFileTest, FaccessFlagsPrecedePathAndUseOnlyLowCarrierBits) {
  Options.reset();
  for (uint32_t Flags : {1u, 0x40u, 0x400u, UINT32_MAX})
    error(ServiceKind::FaccessAt, {UINT64_MAX, 1, UINT64_MAX, Flags}, 22);
  EXPECT_FALSE(invoke(ServiceKind::Access, {1, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  EXPECT_FALSE(invoke(ServiceKind::FaccessAt, {UINT64_MAX, 1, 0, 0x830}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  Options.emplace();
  Options->DescriptorLimit = 3;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/");
  for (uint32_t Flags :
       {0u, 0x10u, 0x20u, 0x30u, 0x800u, 0x810u, 0x820u, 0x830u}) {
    EXPECT_EQ(ok(ServiceKind::FaccessAt,
                 {UINT64_MAX, Base, 0, 0xfedcba9800000000ULL | Flags}),
              0u);
    error(ServiceKind::FaccessAt, {UINT64_MAX, 1, 0, Flags}, 14);
  }
  path("/missing");
  error(ServiceKind::Access, {Base}, 2);
  // No available FD is required, and no query consumes a descriptor.
  error(ServiceKind::Open, {Base}, 24);
}

TEST_P(DarwinFileTest, AccessRelativePathsRetainFDAndAncestorErrorOrder) {
  const auto File = ok(ServiceKind::Open, {Base});
  path("/", Base + 128);
  const auto Directory = ok(ServiceKind::Open, {Base + 128});
  path("data");
  EXPECT_FALSE(invoke(ServiceKind::Access, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {Directory, Base, 0, 0x830}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::FaccessAt, {0x1234567800000000ULL | Directory, Base}),
      0u);
  error(ServiceKind::FaccessAt, {File, Base}, 20);
  error(ServiceKind::FaccessAt, {UINT64_MAX, Base}, 9);
  EXPECT_FALSE(invoke(ServiceKind::FaccessAt, {1, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  for (const char *Text : {"data/", "data/..", "data/.", "data/child"}) {
    path(Text);
    error(ServiceKind::FaccessAt, {Directory, Base}, 20);
  }
  path("missing/..");
  error(ServiceKind::FaccessAt, {Directory, Base}, 2);
  path("");
  error(ServiceKind::FaccessAt, {Directory, Base}, 2);
  error(ServiceKind::FaccessAt, {File, Base}, 20);
  error(ServiceKind::FaccessAt, {UINT64_MAX, Base}, 9);
  error(ServiceKind::FaccessAt, {UINT64_MAX, 1}, 14);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  path("data");
  error(ServiceKind::FaccessAt, {Directory, Base}, 9);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {Directory, Base}), 0u);
}

TEST_P(DarwinFileTest, ExistenceDoesNotReadMetadataOrAlterDescriptionState) {
  mutationPolicy();
  Options->Metadata["/data"].Mode = 0x8000; // Observation, not an access grant.
  Options->DescriptorLimit = 5;
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  const auto Dup = ok(ServiceKind::Dup, {FD});
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
    EXPECT_EQ(ok(ServiceKind::FaccessAt, {Dup, Base}), 0u);
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 2u);
  }
  error(ServiceKind::Open, {Base}, 24);
  error(ServiceKind::Write, {FD, 1, 1}, 14);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Access, {Base, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
}

TEST_P(DarwinFileTest, AccessUsesLiveNamesAcrossRemovalReuseAndRename) {
  mutationPolicy();
  creationPolicy();
  const auto Old = ok(ServiceKind::Open, {Base});
  auto Lease = Files->mappingSource(Old);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.size(), 6u);
  contents(Old, {'a', 'b', 0, 0xff, 'e', 'f'});
  const auto New = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  contents(New, {});
  renameFile("/data", "/moved");
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base + 128}), 0u);
  error(ServiceKind::Access, {Base + 128}, 2);
  path("/");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
}

TEST_P(DarwinFileTest, AccessStringCopyStopsAtNULAndPreservesPathFaults) {
  const auto End = Base + Page * 2;
  path("/data", End - 6);
  EXPECT_EQ(ok(ServiceKind::Access, {End - 6}), 0u);
  ASSERT_FALSE(bool(Space->write(End - 1, {'x'})));
  error(ServiceKind::Access, {End - 6}, 14);
  error(ServiceKind::Access, {UINT64_MAX}, 14);
  error(ServiceKind::Access, {0x800000000000ULL}, 14);
  ASSERT_FALSE(bool(Space->write(Base, std::vector<uint8_t>(1024, 'x'))));
  error(ServiceKind::Access, {Base}, 63);
  path("/" + std::string(256, 'x'));
  error(ServiceKind::Access, {Base}, 63);
  path("/data");
  ASSERT_FALSE(bool(Space->protect(Base, Page, Read | UserAccessible)));
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base, Page, Write | UserAccessible)));
  error(ServiceKind::Access, {Base}, 14);
}

TEST_P(DarwinFileTest, RenameMovesSharedIdentityAndRetainsReplacedObjects) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/target"] = {'T', 'G'};
  Options->WritableFiles.insert("/target");
  auto TargetMetadata = darwin_test::mutationMetadata(2);
  TargetMetadata.Inode++;
  TargetMetadata.Blocks = 1;
  Options->Metadata["/target"] = TargetMetadata;
  Options->MutationPolicies["/target"] = {512, {9, 42}};
  const auto A = ok(ServiceKind::Open, {Base, 0x100000a});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  auto SourceBefore = status(A);
  path("/target");
  const auto T = ok(ServiceKind::Open, {Base, 2});
  auto TargetBefore = status(T);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4, 0}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {T, 1, 0}), 1u);
  auto SourceLease = Files->mappingSource(A);
  auto TargetLease = Files->mappingSource(T);
  renameFile("/data", "/moved");
  for (auto FD : {A, B, D})
    identity(FD, "/moved");
  llvm::support::endian::write64le(SourceBefore.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(SourceBefore.data() + 72, 123456789);
  EXPECT_EQ(status(B), SourceBefore);
  EXPECT_EQ(status(T), TargetBefore);
  renameFile("/moved", "/target");
  llvm::support::endian::write16le(TargetBefore.data() + 6, 0);
  llvm::support::endian::write64le(TargetBefore.data() + 64, 9);
  llvm::support::endian::write64le(TargetBefore.data() + 72, 42);
  EXPECT_EQ(status(A), SourceBefore);
  EXPECT_EQ(status(T), TargetBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {T, 0, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {T, 3}), 2u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(SourceLease).Bytes[0], 'a');
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes[0], 'T');
  contents(B, {'a', 'b', 0, 0xff, 'e', 'f'});
  contents(T, {'T', 'G'});
  renameFile("/target", "/next");
  for (auto FD : {A, B, D})
    identity(FD, "/next");
  identity(T, "/target");
  path("/data");
  error(ServiceKind::Open, {Base}, 2);
  path("/target");
  error(ServiceKind::Open, {Base}, 2);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  SourceLease = TargetLease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {T, 0}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(T).data() + 6), 0u);
  EXPECT_EQ(Options->Files.at("/data").size(), 6u);
  EXPECT_EQ(Options->Metadata.at("/target").LinkCount, 1u);
  EXPECT_EQ(Options->MutationPolicies.at("/target").Time.Seconds, 9);
}

TEST_P(DarwinFileTest, RenameNoopPreservesObservationsAndCreationSequence) {
  mutationPolicy();
  creationPolicy();
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  auto Lease = Files->mappingSource(A);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 3, 0}), 3u);
  renameFile("/data", "/./data");
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(Parent), ParentBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  identity(A, "/data");
  path("/new");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(B).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest, RenameEvenNoopRequiresNamespaceAuthority) {
  Options->Metadata["/data"] = darwin_test::mutationMetadata(6);
  Options->Metadata["/data"].Flags = 2;
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/./data", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  EXPECT_EQ(status(A), Before);
  identity(A, "/data");
}

TEST_P(DarwinFileTest,
       RenameExhaustedInodesAndVirginMetadataRemainIndependent) {
  creationPolicy(UINT64_MAX - 1);
  path("/source");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0755});
  auto Source = status(A);
  path("/target");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  auto Target = status(B);
  renameFile("/source", "/target");
  for (auto *Bytes : {&Source, &Target}) {
    llvm::support::endian::write64le(Bytes->data() + 64, uint64_t(-7));
    llvm::support::endian::write64le(Bytes->data() + 72, 123456789);
  }
  llvm::support::endian::write16le(Target.data() + 6, 0);
  EXPECT_EQ(status(A), Source);
  EXPECT_EQ(status(B), Target);
  path("x", Base + 512);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + 512, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 0}), 0u);
  llvm::support::endian::write64le(Source.data() + 96, 1);
  llvm::support::endian::write64le(Source.data() + 104, 8);
  for (auto *Bytes : {&Source, &Target}) {
    llvm::support::endian::write64le(Bytes->data() + 48, uint64_t(-7));
    llvm::support::endian::write64le(Bytes->data() + 56, 123456789);
  }
  EXPECT_EQ(status(A), Source);
  EXPECT_EQ(status(B), Target);
  renameFile("/target", "/source");
  identity(A, "/source");
  identity(B, "/target");
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  path("/fresh");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationInode);
}

TEST_P(DarwinFileTest, RenameBalancesDynamicPathsAndRetiredTargetCredit) {
  for (bool Dynamic : {false, true}) {
    for (unsigned Retained = 0; Retained != 4; ++Retained) {
      SCOPED_TRACE(testing::Message() << Dynamic << ":" << Retained);
      Options.emplace();
      Options->Files["/data"] = std::vector<uint8_t>(6);
      Options->WritableFiles.insert("/data");
      Options->MutableDirectories.insert("/");
      if (!Dynamic)
        Options->Files["/target"] =
            std::vector<uint8_t>(darwin_file_limits::Bytes - 35);
      Files = std::make_unique<DarwinFiles>(*Space, Options);
      path("/data");
      const auto A = ok(ServiceKind::Open, {Base, 2});
      path("/target");
      const auto T = ok(ServiceKind::Open, {Base, Dynamic ? 0x202u : 0u});
      if (Dynamic)
        EXPECT_EQ(
            ok(ServiceKind::Ftruncate, {T, darwin_file_limits::Bytes - 35}),
            0u);
      DarwinFiles::MappingSource Lease = uint32_t(0);
      if (Retained == 2)
        Lease = Files->mappingSource(T);
      if (Retained != 1 && Retained != 3)
        EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
      path("/data");
      path("/target", Base + 128);
      if (Retained) {
        // Exactly seven spare bytes cannot admit the eight-byte new path.
        EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
        EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
        identity(A, "/data");
        EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 5}), 0u);
      }
      EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
      identity(A, "/target");
      if (Retained) {
        EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 6}));
        EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
      }
      if (Retained == 1)
        EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
      if (Retained == 3)
        EXPECT_EQ(ok(ServiceKind::Dup2, {A, T}), T);
      Lease = uint32_t(0);
      // Initial paths/grants/references stay charged: 14 or 22 bytes.
      const uint64_t Maximum =
          darwin_file_limits::Bytes - (Dynamic ? 14 : 22) - 8;
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Maximum}), 0u);
      EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Maximum + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
      renameFile("/target", "/x");
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Maximum + 5}), 0u);
      path("/x");
      path("/target", Base + 128);
      EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Maximum}), 0u);
      EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
      EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Maximum + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    }
  }
}

TEST_P(DarwinFileTest, RenameKeepsEntryCountAndNeedsNoFreeDescriptor) {
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  renameFile("/f0", "/new");
  path("/another");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  Options->DescriptorLimit = 3;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  renameFile("/f0", "/new");
  path("/new");
  error(ServiceKind::Open, {Base}, 24);
}

TEST_P(DarwinFileTest, RenamePreservesOriginalPathErrorsAndFlagOrdering) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Directories.insert("/folder");
  Options->WorkingDirectory = "/";
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto Before = status(A);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  path("/missing");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 2);
  error(ServiceKind::Rename, {UINT64_MAX, UINT64_MAX}, 14);
  path("/data/");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 20);
  path("/data");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 14);
  for (auto [Name, Code] : {std::pair{"/missing/../new", 2u},
                            {"/data/../new", 20u},
                            {"/new/", 2u},
                            {"/new/.", 2u},
                            {"/data/", 20u},
                            {"/folder", 21u},
                            {"/folder/", 21u},
                            {"/folder/.", 22u},
                            {"/folder/.//", 22u},
                            {"/folder/..", 22u},
                            {"/folder/..//", 22u}}) {
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, Code);
  }
  path("/" + std::string(256, 'x'), Base + 128);
  error(ServiceKind::Rename, {Base, Base + 128}, 63);
  path("");
  error(ServiceKind::RenameAt, {999, Base, Parent, UINT64_MAX}, 9);
  error(ServiceKind::RenameAt, {999, UINT64_MAX, Parent, UINT64_MAX}, 14);
  path("data");
  error(ServiceKind::RenameAt, {A, Base, Parent, UINT64_MAX}, 20);
  path("", Base + 128);
  error(ServiceKind::RenameAt, {Parent, Base, 999, Base + 128}, 9);
  error(ServiceKind::RenameAt, {Parent, Base, 999, UINT64_MAX}, 14);
  path("new", Base + 128);
  error(ServiceKind::RenameAt, {Parent, Base, A, Base + 128}, 20);
  for (uint64_t Flags : {8u, 6u, 0x80000000u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          22);
  for (uint64_t Flags : {1u, 0x11u}) {
    EXPECT_FALSE(invoke(ServiceKind::RenameAtX,
                        {999, UINT64_MAX, 999, UINT64_MAX, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameFlags);
  }
  for (uint64_t Flags : {2u, 0x12u, 4u, 0x14u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          14);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(Parent), ParentBefore);
  path("/renamed", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {Parent, Base, 0x12345678000003e7ULL,
                                        Base + 128, 0x1234567800000010ULL}),
            0u);
  identity(A, "/renamed");
  path("/renamed");
  EXPECT_EQ(ok(ServiceKind::RenameAt, {999, Base, 999, Base + 128}), 0u);
}

TEST_P(DarwinFileTest, ExclusiveRenameChecksExistingTargetsBeforeMutation) {
  mutationPolicy();
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/target"] = {'t'};
  auto TargetMetadata = darwin_test::mutationMetadata(1);
  TargetMetadata.Inode++;
  TargetMetadata.Device++;
  Options->Metadata["/target"] = TargetMetadata;
  Options->Directories.insert("/folder");
  Options->Files["/other/target"] = {'o'};
  const auto A = ok(ServiceKind::Open, {Base, 2});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  const auto Before = status(A);
  path("/target");
  const auto B = ok(ServiceKind::Open, {Base});
  const auto TargetBefore = status(B);
  const auto Lease = Files->mappingSource(B);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  const auto RootBefore = status(Root);
  path("/data");
  // Distinct existing targets fail even without a mutable parent, or when a
  // later mount/device decision would otherwise be outside the model.
  for (uint64_t Flags : {4u, 0x14u}) {
    for (const char *Name :
         {"/target", "/folder", "/folder/", "/other/target"}) {
      path(Name, Base + 128);
      error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}, 17);
    }
    for (auto [Name, Code] : {std::pair{"/folder/.", 22u},
                              {"/folder/..", 22u},
                              {"/new/", 2u},
                              {"/target/", 20u},
                              {"/missing/../target", 2u}}) {
      path(Name, Base + 128);
      error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}, Code);
    }
    error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, Flags}, 14);
  }
  path("/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/other/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(B), TargetBefore);
  EXPECT_EQ(status(Root), RootBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 2u);
  identity(A, "/data");
  identity(B, "/target");
  contents(B, {'t'});
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.front(), 't');
}

TEST_P(DarwinFileTest, ExclusiveRenameNeverGuessesSameObjectCaseSensitivity) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  for (const char *Name : {"/data", "/./data", "//data"}) {
    path(Name, Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 0x14}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameCaseSensitivity);
    EXPECT_EQ(status(A), Before);
    identity(A, "/data");
    // The original unflagged operation remains an admitted no-op.
    EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
    EXPECT_EQ(status(A), Before);
  }
}

TEST_P(DarwinFileTest, ExclusiveRenameRetainsDescriptionsLeasesAndMetadata) {
  mutationPolicy();
  creationPolicy();
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  const auto Lease = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  auto Before = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  path("/renamed", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {0x12345678000003e7ULL, Base, 999,
                                        Base + 128, 0x1234567800000014ULL}),
            0u);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
  for (const auto FD : {A, D}) {
    identity(FD, "/renamed");
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), Flags);
  }
  llvm::support::endian::write64le(Before.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Before.data() + 72, 123456789);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Options->Files.at("/data")));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  contents(D, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_TRUE(Options->Files.contains("/data"));
  EXPECT_FALSE(Options->Files.contains("/renamed"));
}

TEST_P(DarwinFileTest, ExclusiveRenameUsesTheExistingBoundedTransaction) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  path("/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, "/data");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  error(ServiceKind::Access, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 5}), 0u);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}), 0u);
  identity(A, "/new");
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity - 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, CrossParentRenameRetainsDescriptionsAndFileMetadata) {
  mutationPolicy();
  creationPolicy();
  const auto Left = makeDirectory("/left");
  makeDirectory("/right");
  const auto Nested = makeDirectory("/right/nested");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  const auto I = ok(ServiceKind::Open, {Base});
  const auto Lease = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  auto Before = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  path("item", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, Left, Base + 128, 0x14}),
            0u);
  error(ServiceKind::Access, {Base}, 2);
  path("item");
  path("moved", Base + 128);
  EXPECT_EQ(
      ok(ServiceKind::RenameAt, {0x1234567800000000ULL | Left, Base,
                                 0x1234567800000000ULL | Nested, Base + 128}),
      0u);
  llvm::support::endian::write64le(Before.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Before.data() + 72, 123456789);
  for (const auto FD : {A, D, I}) {
    identity(FD, "/right/nested/moved");
    EXPECT_EQ(status(FD), Before);
  }
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Flags);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {I, 0, 1}), 0u);
  path("moved");
  path("/again", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX,
               {Nested, Base, 999, Base + 128, 0x1234567800000014ULL}),
            0u);
  identity(A, "/again");
  contents(I, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Options->Files.at("/data")));
  EXPECT_TRUE(Options->Files.contains("/data"));
  EXPECT_FALSE(Options->Files.contains("/again"));
}

TEST_P(DarwinFileTest, CrossParentReplacementRetainsEachObjectsLastName) {
  mutationPolicy();
  creationPolicy();
  makeDirectory("/left");
  makeDirectory("/right");
  path("/right/target");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("old", Base + Page);
  EXPECT_EQ(ok(ServiceKind::Write, {T, Base + Page, 3}), 3u);
  const auto TargetBefore = status(T);
  const auto TargetLease = Files->mappingSource(T);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(TargetLease));
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto SourceBefore = status(A);
  renameFile("/data", "/right/target");
  auto Removed = TargetBefore;
  llvm::support::endian::write16le(Removed.data() + 6, 0);
  EXPECT_EQ(status(T), Removed);
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            llvm::support::endian::read64le(SourceBefore.data() + 8));
  renameFile("/right/target", "/left/moved");
  identity(T, "/right/target");
  identity(A, "/left/moved");
  contents(T, {'o', 'l', 'd'});
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes,
            llvm::ArrayRef<uint8_t>({'o', 'l', 'd'}));
  path("/right/target");
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, CrossParentRenameDoesNotInferMountsFromDeviceNumbers) {
  Options->Directories.insert("/other");
  Options->MutableDirectories = {"/", "/other"};
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  auto Other = darwin_test::creationParentMetadata();
  Other.Inode++;
  Options->Metadata["/other"] = Other;
  makeDirectory("/created");
  makeDirectory("/other/created");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  for (const char *Name : {"/other/new", "/other/created/new"}) {
    path(Name, Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
    error(ServiceKind::Access, {Base + 128}, 2);
  }
  identity(A, "/data");
  path("/other/created/item");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  renameFile("/other/created/item", "/other/item");
  identity(B, "/other/item");
  path("/other/item");
  path("/created/item", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(B, "/other/item");
}

TEST_P(DarwinFileTest, CrossParentRenameUsesObjectsAfterInitialNameReuse) {
  Options->Directories.insert("/same");
  Options->MutableDirectories = {"/", "/same"};
  Options->RemovableDirectories.insert("/same");
  path("/same");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto OldChild = makeDirectory("/same/child");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/same/child/item", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  path("/same/child");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/same");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  makeDirectory("/same");
  const auto NewChild = makeDirectory("/same/child");
  path("/data");
  path("item", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, NewChild, Base + 128, 4}),
            0u);
  identity(A, "/same/child/item");
  path("item");
  error(ServiceKind::OpenAt, {OldChild, Base}, 2);
  EXPECT_NE(ok(ServiceKind::OpenAt, {NewChild, Base}), UINT64_MAX);
  path("child/item");
  error(ServiceKind::OpenAt, {Old, Base}, 2);
  identity(OldChild, "/same/child");
}

TEST_P(DarwinFileTest, CrossParentRenameKeepsTheSourcesWriteAuthority) {
  Options->MutableDirectories.insert("/");
  makeDirectory("/new");
  path("/new/target");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  renameFile("/data", "/new/target");
  path("/new/target");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  error(ServiceKind::Ftruncate, {A, 0}, 22);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {T, 1}), 0u);
  contents(T, {0});
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
}

TEST_P(DarwinFileTest, CrossParentRenamePreservesLookupAndExclusiveOrder) {
  mutationPolicy();
  creationPolicy();
  const auto New = makeDirectory("/new");
  makeDirectory("/new/sub");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  for (auto [Name, Code] : {std::pair{"/new/.", 22u},
                            {"/new/sub/..", 22u},
                            {"/new/missing/../x", 2u},
                            {"/new/x/", 2u}}) {
    path(Name, Base + 128);
    for (uint64_t Flags : {0u, 0x14u})
      error(ServiceKind::RenameAtX, {999, Base, New, Base + 128, Flags}, Code);
  }
  for (const char *Name : {"/new", "/new/"}) {
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, 21);
    error(ServiceKind::RenameAtX, {999, Base, New, Base + 128, 4}, 17);
  }
  error(ServiceKind::RenameAt, {999, Base, New, UINT64_MAX}, 14);
  path("missing");
  error(ServiceKind::RenameAt, {New, Base, New, UINT64_MAX}, 2);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  EXPECT_EQ(status(A), Before);
  identity(A, "/data");
}

TEST_P(DarwinFileTest, CrossParentRenameRejectsContradictoryOwnedDevices) {
  mutationPolicy();
  creationPolicy();
  Options->Metadata["/data"].Device++;
  makeDirectory("/new");
  path("/new/target");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  const auto TargetBefore = status(T);
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/new/moved", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  error(ServiceKind::Access, {Base + 128}, 2);
  path("/new/target", Base + 128);
  error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}, 17);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(T), TargetBefore);
  identity(A, "/data");
  identity(T, "/new/target");
}

TEST_P(DarwinFileTest, CrossParentRenameChargesTheCompleteNewPath) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  makeDirectory("/n");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  // Initial path/grants reserve 14 bytes; the created directory reserves 3.
  const auto Capacity = darwin_file_limits::Bytes - 17;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  path("/n/x", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, "/data");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  error(ServiceKind::Access, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 5}), 0u);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}), 0u);
  identity(A, "/n/x");
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity - 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, CrossParentReplacementCreditsOnlyReleasedMappingLeases) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  makeDirectory("/n");
  path("/n/x");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("old", Base + Page);
  EXPECT_EQ(ok(ServiceKind::Write, {T, Base + Page, 3}), 3u);
  auto Lease = Files->mappingSource(T);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  // 17 fixed bytes, plus the target's five-byte path and three-byte contents.
  const auto Capacity = darwin_file_limits::Bytes - 25;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  path("/n/x", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, "/data");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'o', 'l', 'd'}));
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  identity(A, "/n/x");
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity + 3}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity + 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, RenameDirectoryAndUnknownMountMovesRemainExplicit) {
  Options->MutableDirectories = {"/", "/folder"};
  Options->Directories.insert("/folder");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  auto Other = darwin_test::creationParentMetadata();
  Other.Inode++;
  Options->Metadata["/folder"] = Other;
  const auto A = ok(ServiceKind::Open, {Base});
  path("/folder/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(A, "/data");
  path("/folder");
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  path("/new/", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
}

TEST_P(DarwinFileTest, RenameNestedDotTargetsFailBeforeNamespaceAdmission) {
  Options->Files["/dir/data"] = {'x'};
  Options->Directories = {"/dir/sub", "/other"};
  // These lookup failures precede both namespace grants and mount checks.
  path("/dir/data");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/dir");
  const auto D = ok(ServiceKind::Open, {Base});
  path("/dir/data");
  for (auto [Name, Code] : {std::pair{"/dir/.", 22u},
                            {"/dir/sub/..", 22u},
                            {"/dir/..", 22u},
                            {"/other/.", 22u},
                            {"/other/..//", 22u},
                            {"/dir/missing/.", 2u},
                            {"/dir/data/..", 20u}}) {
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, Code);
  }
  path("data");
  for (const char *Name : {".", "sub/..", "..//"}) {
    path(Name, Base + 128);
    error(ServiceKind::RenameAt, {D, Base, D, Base + 128}, 22);
  }
  identity(A, "/dir/data");
  contents(A, {'x'});
  path("/dir/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), D + 1);
}

TEST_P(DarwinFileTest, RenameChecksOwnedDeviceAfterAnOldNameIsReused) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/old"] = {};
  auto Other = darwin_test::mutationMetadata(0);
  Other.Inode++;
  Other.Device = 456;
  Options->Metadata["/old"] = Other;
  const auto A = ok(ServiceKind::Open, {Base, 2});
  path("/old", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(A, "/data");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base + 128}), 0u);
  renameFile("/data", "/old");
  // Whole-EFAULT loses the full stat record, but cannot change its device.
  error(ServiceKind::Write, {A, UINT64_MAX, 1}, 14);
  renameFile("/old", "/new");
  identity(A, "/new");
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {A, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
  EXPECT_EQ(Options->Metadata.at("/old").Device, 456);
}

TEST_P(DarwinFileTest, RenameNeverInheritsTargetWriteAuthority) {
  Options->MutableDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  Options->WritableFiles.insert("/target");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/target");
  const auto T = ok(ServiceKind::Open, {Base, 2});
  renameFile("/data", "/target");
  path("/target");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  error(ServiceKind::Ftruncate, {A, 0}, 22);
  EXPECT_FALSE(invoke(ServiceKind::Truncate, {Base, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {T, 0}), 0u);
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
}

TEST_P(DarwinFileTest, RenameCanonicalPathLimitPreservesRelativeSource) {
  std::string Parent;
  for (unsigned I = 0; I != 4; ++I)
    Parent += '/' + std::string(250, 'a');
  Parent += '/' + std::string(10, 'b');
  Options.emplace();
  Options->Files[Parent + "/s"] = {'x'};
  Options->MutableDirectories.insert(Parent);
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path(Parent);
  const auto P = ok(ServiceKind::Open, {Base});
  path("s");
  const auto A = ok(ServiceKind::OpenAt, {P, Base});
  path("0123456789", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAt, {P, Base, P, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, Parent + "/s");
  EXPECT_EQ(ok(ServiceKind::OpenAt, {P, Base}), A + 1);
}

TEST_P(DarwinFileTest, UmaskKeepsAllPermissionBitsWithoutCreationAuthority) {
  EXPECT_FALSE(invoke(ServiceKind::Umask, {0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileUmask);
  Options->InitialUmask = 0027;
  EXPECT_EQ(ok(ServiceKind::Umask, {0x12345678000001edULL}), 0027u);
  EXPECT_EQ(ok(ServiceKind::Umask, {07000}), 0755u);
  const auto A = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Umask, {UINT64_MAX}), 07000u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Umask, {0}), 07777u);
  EXPECT_EQ(ok(ServiceKind::Umask, {0022}), 0u);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
  EXPECT_FALSE(Options->CreationPolicy);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
}

TEST_P(DarwinFileTest, CreationMetadataOwnsIdentityAndUsesCurrentUmask) {
  creationPolicy();
  EXPECT_EQ(ok(ServiceKind::Umask, {07027}), 0027u);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  path("new");
  const auto A =
      ok(ServiceKind::OpenAt, {Parent, Base, 0xe02, 0x1234567800000fffULL});
  auto Bytes = status(A);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read16le(Bytes.data() + 4), 0100750u);
  EXPECT_EQ(llvm::support::endian::read16le(Bytes.data() + 6), 1u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8),
            0xfedcba9876543211ULL);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 16), 1000u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 96), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 104), 0u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 112), 8192u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 116), 0u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 120), 0x89abcdefu);
  for (unsigned Offset : {32u, 48u, 64u, 80u}) {
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + Offset),
              uint64_t(-19));
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + Offset + 8),
              987654321u);
  }
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 2u);
  error(ServiceKind::Lseek, {A, 0, 4}, 6);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Umask, {07777}), 07027u);
  path("second");
  const auto B = ok(ServiceKind::OpenAt, {Parent, Base, 0x202, 07777});
  const auto Second = status(B);
  EXPECT_EQ(llvm::support::endian::read16le(Second.data() + 4), 0100000u);
  EXPECT_EQ(llvm::support::endian::read64le(Second.data() + 8),
            0xfedcba9876543212ULL);
  EXPECT_EQ(llvm::support::endian::read32le(Second.data() + 20), 0xfedcba98u);
  EXPECT_EQ(status(A), Bytes);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(Options->CreationPolicy->FirstInode, 0xfedcba9876543211ULL);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest, CreationPolicySharesSparseMutationAndUnlinkState) {
  creationPolicy();
  path("/new");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0666});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  const auto Initial = status(A);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 8193}), 0u);
  auto Expected = Initial;
  llvm::support::endian::write64le(Expected.data() + 96, 8193);
  for (unsigned Offset : {48u, 64u}) {
    llvm::support::endian::write64le(Expected.data() + Offset, uint64_t(-7));
    llvm::support::endian::write64le(Expected.data() + Offset + 8, 123456789);
  }
  EXPECT_EQ(status(B), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 3}), 0u);
  error(ServiceKind::Lseek, {B, 0, 4}, 6);
  path("x", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {D, Base + 128, 1, 4096}), 1u);
  llvm::support::endian::write64le(Expected.data() + 104, 8);
  EXPECT_EQ(status(B), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 4}), 4096u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4096, 3}), 8192u);
  auto Lease = Files->mappingSource(A);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  EXPECT_EQ(status(D), Expected);
  const auto Fresh = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(Fresh).data() + 8),
            0xfedcba9876543212ULL);
  EXPECT_EQ(ok(ServiceKind::Write, {Fresh, Base + 128, 1}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 0}), 0u);
  llvm::support::endian::write64le(Expected.data() + 96, 0);
  llvm::support::endian::write64le(Expected.data() + 104, 0);
  EXPECT_EQ(status(B), Expected);
  error(ServiceKind::Write, {Fresh, UINT64_MAX, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {Fresh, Base + 128, 1, 0}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Fresh, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Fresh, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
}

TEST_P(DarwinFileTest, CreationInodesAreGlobalAndParentFieldsStayIndependent) {
  creationPolicy();
  Options->Directories.insert("/parent");
  Options->MutableDirectories.insert("/parent");
  auto Other = darwin_test::creationParentMetadata();
  Other.Device = 456;
  Other.GID = 123;
  Other.Inode = 42;
  Options->Metadata["/parent"] = Other;
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("/parent");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  const char *Names[] = {"/one", "/parent/two", "/three"};
  for (unsigned I = 0; I != 3; ++I) {
    path(Names[I]);
    const auto A = ok(ServiceKind::Open, {Base, 0x202, 0600});
    const auto Bytes = status(A);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8),
              darwin_test::CreationPolicy.FirstInode + I);
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data()),
              I == 1 ? 456u : uint32_t(-123));
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 20),
              I == 1 ? 123u : 0xfedcba98u);
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    if (!I)
      EXPECT_EQ(status(Parent), ParentBefore);
    else {
      EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    }
    EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  }
}

TEST_P(DarwinFileTest, CreationUnlinkBeforeFirstWriteKeepsOwnedMetadata) {
  creationPolicy();
  path("/new");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0755});
  auto Expected = status(A);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  llvm::support::endian::write64le(Expected.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 72, 123456789);
  EXPECT_EQ(status(A), Expected);
  path("x", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 1, 4096}), 1u);
  llvm::support::endian::write64le(Expected.data() + 48, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 56, 123456789);
  llvm::support::endian::write64le(Expected.data() + 96, 4097);
  llvm::support::endian::write64le(Expected.data() + 104, 8);
  EXPECT_EQ(status(A), Expected);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 8193}), 0u);
  llvm::support::endian::write64le(Expected.data() + 96, 8193);
  EXPECT_EQ(status(A), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 4}), 4096u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4096, 3}), 8192u);
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, CreationBudgetRefusalsPreserveInodeAndParentMetadata) {
  creationPolicy();
  Options->WritableFiles.insert("/data");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto Before = status(Parent);
  // Data path/reference and parent metadata/grant cost 16 bytes.
  const auto Capacity = darwin_file_limits::Bytes - 16;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 4}), 0u);
  path("new");
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {Parent, Base, 0xa02, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  EXPECT_EQ(status(Parent), Before);
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  const auto A = ok(ServiceKind::OpenAt, {Parent, Base, 0xa02, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest, CreationEntryRefusalsDoNotConsumeInodeSequence) {
  creationPolicy();
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  path("/f0");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest, CreationInodesNeverRecycleAfterExhaustionOrFailedOpens) {
  creationPolicy(UINT64_MAX);
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {Base, 0xe02, 0600}, 17);
  const auto Original = ok(ServiceKind::Open, {Base});
  path("/new");
  error(ServiceKind::Open, {Base, 0x202, 0600}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {Original}), 0u);
  error(ServiceKind::Open, {UINT64_MAX, 0x202}, 14);
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8), UINT64_MAX);
  EXPECT_EQ(ok(ServiceKind::Umask, {0}), 0027u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0x200, 0777});
  EXPECT_EQ(llvm::support::endian::read64le(status(B).data() + 8), UINT64_MAX);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  EXPECT_EQ(ok(ServiceKind::Umask, {0022}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationInode);
  error(ServiceKind::Open, {Base}, 2);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x200}), Original);
  EXPECT_EQ(Options->CreationPolicy->FirstInode, UINT64_MAX);
}

TEST_P(DarwinFileTest, CreateDistinguishesNewTruncateAndDescriptionFlags) {
  Options->MutableDirectories.insert("/");
  for (uint32_t Access : {0u, 1u, 2u}) {
    path("/new" + std::to_string(Access));
    const auto A = ok(ServiceKind::Open, {Base, 0x1000608u | Access, 0666});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), Access | 8u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 2}), 0u);
    const auto B = ok(ServiceKind::Open, {Base, 2});
    const auto D = ok(ServiceKind::Dup, {B});
    path("xyz", Base + 128);
    EXPECT_EQ(ok(ServiceKind::Write, {B, Base + 128, 3}), 3u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 3u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
    contents(B, {'x', 'y', 'z'});
    if (!Access)
      error(ServiceKind::Write, {A, Base + 128, 1}, 9);
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, UINT64_MAX}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {B, 0, 4}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
    const auto T = ok(ServiceKind::Open, {Base, 0x600});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {T, 3}), 0x10000u);
    contents(T, {});
    for (auto FD : {A, B, D, T})
      EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  }
  EXPECT_EQ(Options->Files.size(), 1u);
  EXPECT_TRUE(Options->WritableFiles.empty());
}

TEST_P(DarwinFileTest, CreateExistingNamesPreservesAuthorityAndExclusiveOrder) {
  Options->Directories.insert("/empty");
  error(ServiceKind::Open, {Base, 0xe02}, 17);
  const auto A = ok(ServiceKind::Open, {Base, 0x200});
  auto Source = Files->mappingSource(A);
  error(ServiceKind::Open, {Base, 0xe02}, 17);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x602}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0x800});
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  path("/absent");
  error(ServiceKind::Open, {Base, 0x800}, 2);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/empty/.");
  error(ServiceKind::Open, {Base, 0xa02}, 17);
  error(ServiceKind::Open, {Base, 0x202}, 21);
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x200}), B);
}

TEST_P(DarwinFileTest,
       CreateWalksOriginalComponentsAndReservesDescriptorsFirst) {
  Options->MutableDirectories.insert("/");
  Options->WorkingDirectory = "/";
  Options->DescriptorLimit = 5;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  error(ServiceKind::Open, {UINT64_MAX, 0x100200}, 22);
  error(ServiceKind::OpenAt, {999, UINT64_MAX, 0x200}, 14);
  path("");
  error(ServiceKind::OpenAt, {999, Base, 0x200}, 9);
  error(ServiceKind::Open, {Base, 0x200}, 2);
  for (const char *Name : {"/missing/", "/missing//", "/missing/.",
                           "/missing/..", "/missing/../new"}) {
    path(Name);
    error(ServiceKind::Open, {Base, 0xa02}, 2);
  }
  path("/data/");
  error(ServiceKind::Open, {Base, 0xa02}, 20);
  path("/");
  const auto Directory = ok(ServiceKind::Open, {Base});
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Open, {UINT64_MAX, 0x203}, 22);
  error(ServiceKind::Open, {UINT64_MAX, 0x100200}, 24);
  path("/new");
  error(ServiceKind::Open, {Base, 0x202}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  error(ServiceKind::Open, {Base}, 2);
  path("new");
  error(ServiceKind::OpenAt, {A, Base, 0x200}, 20);
  path("/./new");
  const auto B = ok(ServiceKind::OpenAt, {999, Base, 0xa02});
  EXPECT_EQ(B, Directory);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  path("/new");
  const auto C = ok(ServiceKind::Open, {Base, 0x800});
  contents(C, {});
  EXPECT_EQ(ok(ServiceKind::Close, {C}), 0u);
  path("relative");
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x200}), Directory);
}

TEST_P(DarwinFileTest, CreateInvalidatesParentOnlyAfterInsertion) {
  Options->WritableFiles.insert("/data");
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  auto Root = darwin_test::mutationMetadata(64);
  Root.Inode = 41;
  Root.Mode = 0040755;
  Options->Metadata["/"] = Root;
  Options->MutableDirectories.insert("/");
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto Before = status(Parent);
  path("/data");
  error(ServiceKind::Open, {Base, 0xe02}, 17);
  const auto Existing = ok(ServiceKind::Open, {Base, 0x200});
  EXPECT_EQ(ok(ServiceKind::Close, {Existing}), 0u);
  path("/empty/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  error(ServiceKind::Open, {Base}, 2);
  EXPECT_EQ(status(Parent), Before);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Parent, Base + Page, 64, Base + 512}),
      64u);
  const auto Cursor = ok(ServiceKind::Lseek, {Parent, 0, 1});
  path("/data", Base + 128);
  const auto Data = ok(ServiceKind::Open, {Base + 128, 2});
  // Paths/references cost 23 bytes, and the four LP64 records cost 128.
  EXPECT_EQ(
      ok(ServiceKind::Ftruncate, {Data, darwin_file_limits::Bytes - 151 - 4}),
      0u);
  path("new");
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {Parent, Base, 0xa00}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  EXPECT_EQ(status(Parent), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, 0, 1}), Cursor);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Parent, Base + Page, 64, Base + 512}),
      64u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, Cursor, 0}), Cursor);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
  EXPECT_EQ(ok(ServiceKind::OpenAt, {Parent, Base, 0xa00}), Existing);
  std::array<uint8_t, 160> Canary;
  Canary.fill(0x5a);
  llvm::cantFail(Space->write(Base + Page, Canary));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Parent, Base + Page, 64, Base + Page + 144}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, 0, 1}), Cursor);
  std::array<uint8_t, 160> After;
  llvm::cantFail(Space->read(Base + Page, After));
  EXPECT_EQ(After, Canary);
}

TEST_P(DarwinFileTest, CreateReusesNamesWithoutInheritingObjectsOrPolicies) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  auto Old = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Old));
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0xe02});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  path("new", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {B, Base + 128, 3}), 3u);
  contents(B, {'n', 'e', 'w'});
  contents(D, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(llvm::support::endian::read16le(status(D).data() + 6), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 4}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {B, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  auto Fresh = Files->mappingSource(B);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Fresh));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Old).Bytes.front(), 'a');
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Fresh).Bytes.front(), 'n');
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  Old = uint32_t(0);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  Fresh = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 0}), 0u);
  EXPECT_EQ(Options->Metadata.at("/data").LinkCount, 1u);
  EXPECT_EQ(Options->Files.at("/data").size(), 6u);
}

TEST_P(DarwinFileTest, CreateChargesAndReclaimsDynamicPathsWithObjectLifetime) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  // Initial path, writable reference and mutable root remain charged.
  const uint64_t Capacity = darwin_file_limits::Bytes - 6 - 6 - 2;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 4}), 0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  error(ServiceKind::Open, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 5}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0x202});
  const auto D = ok(ServiceKind::Dup, {B});
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  auto Source = Files->mappingSource(B);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, D}), D);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  Source = uint32_t(0);
  const auto C = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(C, B);
  EXPECT_EQ(ok(ServiceKind::Close, {C}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity}));
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
}

TEST_P(DarwinFileTest, CreateCountsNamedAndOrphanedObjectsAndReclaimsEntries) {
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  path("/f0");
  const auto A = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  for (unsigned I = 0; I != 300; ++I) {
    SCOPED_TRACE(I);
    const auto B = ok(ServiceKind::Open, {Base, 0xa00});
    EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
    path("/extra", Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Open, {Base + 128, 0x200}));
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  }
}

TEST_P(DarwinFileTest, CreateChecksCanonicalBudgetAfterRelativeResolution) {
  std::string Parent;
  for (char C : {'a', 'b', 'c', 'd'})
    Parent += '/' + std::string(240, C);
  Options->Directories.insert(Parent);
  Options->MutableDirectories.insert(Parent);
  Options->WorkingDirectory = Parent;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path(std::string(59, 'x'));
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  error(ServiceKind::Open, {Base}, 2);
  path(std::string(58, 'y'));
  const auto A = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 50, Base + Page}), 0u);
  std::vector<uint8_t> Name(1024);
  llvm::cantFail(Space->read(Base + Page, Name));
  EXPECT_EQ(std::string(reinterpret_cast<char *>(Name.data())),
            Parent + '/' + std::string(58, 'y'));
}

TEST_P(DarwinFileTest, UnlinkRemovesNamesAndPreservesOpenObjectsAndPaths) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto Original = Options->Files.at("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  const auto Before = status(A);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  error(ServiceKind::Open, {Base}, 2);
  error(ServiceKind::Unlink, {Base}, 2);
  error(ServiceKind::Stat64, {Base, UINT64_MAX}, 2);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 2u);
  contents(B, Original);
  auto Expected = Before;
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  llvm::support::endian::write64le(Expected.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 72, 123456789);
  EXPECT_EQ(status(D), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 4}), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 1, 3}), 6u);
  path("Z", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {D, Base + 128, 1, 0}), 1u);
  EXPECT_EQ(llvm::support::endian::read16le(status(B).data() + 6), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 3}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(B).data() + 6), 0u);
  contents(B, {'Z', 'b', 0});
  std::array<uint8_t, 7> Name;
  Name.fill(0x5a);
  llvm::cantFail(Space->write(Base + 256, Name));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 50, Base + 256}), 0u);
  llvm::cantFail(Space->read(Base + 256, Name));
  EXPECT_EQ(Name, (std::array<uint8_t, 7>{'/', 'd', 'a', 't', 'a', 0, 0x5a}));
  error(ServiceKind::Fcntl, {D, 50, UINT64_MAX}, 14);
  for (auto FD : {A, B, D})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Open, {Base}, 2);
  EXPECT_EQ(Options->Files.at("/data"), Original);
  EXPECT_EQ(Options->Metadata.at("/data").LinkCount, 1u);
}

TEST_P(DarwinFileTest,
       UnlinkReadonlyFilesKeepsImplicitParentsAndRelativePaths) {
  Options->Files["/tree/child"] = {'x'};
  Options->MutableDirectories.insert("/tree");
  Options->WorkingDirectory = "/tree";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/tree");
  const auto Directory = ok(ServiceKind::Open, {Base});
  path("child", Base + 128);
  const auto A = ok(ServiceKind::OpenAt, {Directory, Base + 128});
  error(ServiceKind::UnlinkAt, {999, UINT64_MAX, 0x80000000}, 22);
  error(ServiceKind::UnlinkAt, {999, UINT64_MAX, 0}, 14);
  path("", Base + 256);
  error(ServiceKind::UnlinkAt, {999, Base + 256, 0}, 9);
  error(ServiceKind::UnlinkAt, {A, Base + 128, 0}, 20);
  EXPECT_FALSE(invoke(ServiceKind::UnlinkAt, {999, UINT64_MAX, 0x100}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::UnlinkFlags);
  path("/tree/child/", Base + 256);
  error(ServiceKind::Unlink, {Base + 256}, 20);
  EXPECT_EQ(
      ok(ServiceKind::UnlinkAt, {Directory, Base + 128, 0x1234567800000800ULL}),
      0u);
  contents(A, {'x'});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  error(ServiceKind::Open, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
  path(".", Base + 128);
  const auto Parent = ok(ServiceKind::Open, {Base + 128});
  error(ServiceKind::Read, {Parent, 0, 0}, 21);
  error(ServiceKind::Unlink, {Base}, 1);
  path("/");
  error(ServiceKind::Unlink, {Base}, 21);
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  const auto Other = ok(ServiceKind::Open, {Base});
  contents(Other, {'a', 'b', 0, 0xff, 'e', 'f'});
}

TEST_P(DarwinFileTest, UnlinkInvalidatesOnlyItsParentObservations) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  auto Root = darwin_test::mutationMetadata(64);
  Root.Inode = 41;
  Root.Mode = 0040755;
  Options->Metadata["/"] = Root;
  auto Empty = Root;
  Empty.Inode = 42;
  Options->Metadata["/empty"] = Empty;
  Options->MutableDirectories.insert("/");
  path("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  path("/empty", Base + 128);
  const auto B = ok(ServiceKind::Open, {Base + 128});
  const auto EmptyStatus = status(B);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {A, Base + Page, 64, Base + 512}),
            64u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {D, Base + Page, 64, Base + 512}),
            64u);
  const auto Offset = ok(ServiceKind::Lseek, {A, 0, 1});
  error(ServiceKind::Unlink, {UINT64_MAX}, 14);
  EXPECT_EQ(ok(ServiceKind::Fstat64, {D, Base + 256}), 0u);
  path("/data", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base + 128}), 0u);
  const auto Fresh = ok(ServiceKind::Open, {Base});
  llvm::cantFail(Space->writeInteger(Base + 256, 0x7777, 2));
  llvm::cantFail(Space->writeInteger(Base + 512, 0x8888, 2));
  llvm::cantFail(Space->writeInteger(Base + 256 + 1020, 0x9999, 2));
  for (auto FD : {A, D, Fresh}) {
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + 256}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    EXPECT_FALSE(
        invoke(ServiceKind::GetDirEntries64, {FD, Base + 256, 64, UINT64_MAX}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                        {FD, Base + 256, 1024, Base + 512}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {FD, 0, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  }
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0x7777u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 512, 2)), 0x8888u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 1020, 2)), 0x9999u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), Offset);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {A}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(status(B), EmptyStatus);
}

TEST_P(DarwinFileTest, UnlinkBudgetWaitsForEveryDescriptionAndMappingLease) {
  Options->Files["/other"] = {};
  Options->WritableFiles = {"/data", "/other"};
  Options->MutableDirectories.insert("/");
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 2;
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  path("/other", Base + 128);
  const auto B = ok(ServiceKind::Open, {Base + 128, 2});
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  auto Source = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Source));
  const auto Bytes = std::get<DarwinFiles::Mapping>(Source).Bytes;
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Dup2, {B, D}), D);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(Bytes.front(), 'a');
  EXPECT_EQ(Bytes.back(), 0u);
  Source = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 2}), Capacity);
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, UnlinkFirstInitializesBudgetAndReclaimsCurrentSize) {
  Options->Files["/other"] = {};
  Options->WritableFiles = {"/data", "/other"};
  Options->MutableDirectories.insert("/");
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 2;
  // No prior open or mutation: the next growth must reclaim initial bytes.
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/other");
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 7}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 11}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Open, {Base}, 2);
}

TEST(DarwinFileOptions, NamespaceAdmissionRejectsFlagsAndAllKnownAliases) {
  DarwinFileOptions Good;
  Good.Files["/tree/a"] = {};
  Good.MutableDirectories.insert("/tree");
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (auto Path : {"/missing", "/tree/a", "/tree/../tree"}) {
    auto O = Good;
    O.MutableDirectories = {Path};
    auto Error = validateFileOptions(O);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  auto M = darwin_test::mutationMetadata(0);
  Good.Metadata["/tree/a"] = M;
  M.Mode = 0040755;
  M.Inode = 43;
  Good.Metadata["/tree"] = M;
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (auto Path : {"/tree", "/tree/a"}) {
    for (auto Flags : {1u, 2u, 4u, 0x20000u, 0x40000u, 0x100000u}) {
      auto O = Good;
      O.Metadata[Path].Flags = Flags;
      EXPECT_EQ(llvm::toString(validateFileOptions(O)),
                diagnostic::NamespaceFlags);
    }
  }
  for (auto Bits : {01000, 02000, 04000}) {
    auto O = Good;
    O.Metadata["/tree"].Mode |= Bits;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceFlags);
  }
  for (auto Links : {0, 2}) {
    auto O = Good;
    O.Metadata["/tree/a"].LinkCount = Links;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceAlias);
  }
  for (auto Path : {"/tree", "/tree/a"}) {
    auto O = Good;
    if (std::string(Path) == "/tree")
      O.Directories.insert("/alias");
    else
      O.Files["/alias"] = {};
    O.Metadata["/alias"] = O.Metadata[Path];
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceAlias);
    O.Metadata["/alias"].Device++;
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  DarwinFileOptions Snapshot;
  Snapshot.Files["/data"] = {};
  Snapshot.Directories.insert("/empty");
  Snapshot.MutableDirectories.insert("/");
  Snapshot.DirectoryContents["/"] = darwin_test::directoryContents();
  EXPECT_FALSE(bool(validateFileOptions(Snapshot)));
  Snapshot.DirectoryContents["/"].Entries[2].Inode = 41;
  EXPECT_EQ(llvm::toString(validateFileOptions(Snapshot)),
            diagnostic::NamespaceAlias);
}

TEST_P(DarwinFileTest, UnlinkDoesNotRestoreUnknownMetadataOrReuseAbsentNames) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  error(ServiceKind::Write, {FD, UINT64_MAX, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::UnlinkAt, {999, Base, 0}), 0u);
  path("X", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {FD, Base + 128, 1}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {FD, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  path("/");
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(Directory, FD);
  path("data");
  error(ServiceKind::OpenAt, {Directory, Base}, 2);
  path("/data");
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, WritableNodesSurviveSeparateOpensDupAndLastClose) {
  Options->WritableFiles.insert("/data");
  const auto Original = Options->Files.at("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto B = ok(ServiceKind::Open, {Base});
  auto D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 0}), 1u);
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {D, Base + 128, 2}), 2u);
  contents(B, {'a', 'X', 'Y', 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 2, 8}), 2u);
  contents(B, {'a', 'X', 'Y', 0xff, 'e', 'f', 0, 0, 'X', 'Y'});
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 3u);
  for (auto FD : {A, B, D})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  auto Reopened = ok(ServiceKind::Open, {Base});
  contents(Reopened, {'a', 'X', 'Y', 0xff, 'e', 'f', 0, 0, 'X', 'Y'});
  EXPECT_EQ(Options->Files.at("/data"), Original);
}

TEST_P(DarwinFileTest, AppendFlagsAreSharedByDupAndIgnoredByPwrite) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 0x100000a});
  auto D = ok(ServiceKind::Dup, {A});
  auto B = ok(ServiceKind::Open, {Base, 2});
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 4, 0x1000a}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 2, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Write, {D, Base + 128, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 8u);
  contents(B, {'a', 'X', 'Y', 0xff, 'e', 'f', 'X', 'Y'});
  const auto WrittenFlags = ok(ServiceKind::Fcntl, {D, 3});
  EXPECT_EQ(WrittenFlags, 0x1000au);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 4, WrittenFlags & ~8u}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 4, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {A, 4, 4}));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
}

TEST_P(DarwinFileTest, TruncationChangesSharedBytesWithoutMovingCursors) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto B = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 5, 0}), 5u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 3}), 0u);
  contents(B, {'a', 'b', 0});
  EXPECT_EQ(ok(ServiceKind::Truncate, {Base, 8}), 0u);
  contents(B, {'a', 'b', 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 5u);
  auto ReadOnlyTruncate = ok(ServiceKind::Open, {Base, 0x400});
  contents(B, {});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 5u);
  error(ServiceKind::Write, {ReadOnlyTruncate, 0, 0}, 9);
  error(ServiceKind::Ftruncate, {B, 0}, 22);
  error(ServiceKind::Ftruncate, {999, UINT64_MAX}, 22);
  error(ServiceKind::Truncate, {0, UINT64_MAX}, 22);
  path("/");
  error(ServiceKind::Truncate, {Base, 0}, 21);
  error(ServiceKind::Open, {Base, 1}, 21);
  error(ServiceKind::Open, {Base, 3}, 22);
}

TEST_P(DarwinFileTest, TruncationRecordsOnlyTheParticipatingOpenDescription) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto D = ok(ServiceKind::Dup, {A});
  auto B = ok(ServiceKind::Open, {Base, 2});
  error(ServiceKind::Ftruncate, {D, UINT64_MAX}, 22);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 2u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_EQ(ok(ServiceKind::Truncate, {Base, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, darwin_file_limits::Bytes}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  for (unsigned Access : {0u, 1u, 2u}) {
    auto T = ok(ServiceKind::Open, {Base, 0x400 | Access});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {T, 3}), 0x10000u | Access);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
    EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
  }
}

TEST_P(DarwinFileTest, SparseSeekUsesKnownUnitsAndSharesOnlyTheOpenCursor) {
  for (uint32_t Unit : {512u, 4096u, 16384u}) {
    SCOPED_TRACE(Unit);
    mutationPolicy(Unit);
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto A = ok(ServiceKind::Open, {Base, 2});
    const auto D = ok(ServiceKind::Dup, {A});
    const auto B = ok(ServiceKind::Open, {Base});
    const auto Initial = status(A);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 4}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 2, 0x1234567800000003ULL}), 6u);
    EXPECT_EQ(status(A), Initial);
    for (uint64_t Whence : {3u, 4u}) {
      error(ServiceKind::Lseek, {A, UINT64_MAX, Whence}, 22);
      error(ServiceKind::Lseek, {A, 6, Whence}, 6);
      error(ServiceKind::Lseek, {A, INT64_MAX, Whence}, 6);
      EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 6u);
    }
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 0}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Unit * 5 + 3}), 0u);
    error(ServiceKind::Lseek, {A, 0, 4}, 6);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 6u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 3}), 1u);
    llvm::cantFail(Space->writeInteger(Base + 128, 0, 2));
    EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 2, Unit * 2 - 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 4}), Unit);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, Unit + 7, 4}), Unit + 7);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, Unit - 1, 3}), Unit - 1);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, Unit, 3}), Unit * 3);
    error(ServiceKind::Lseek, {A, Unit * 3, 4}, 6);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), Unit * 3);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 1, Unit * 5 + 2}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, Unit * 3, 4}), Unit * 5);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, Unit * 5, 3}), Unit * 5 + 3);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Unit * 2}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Unit * 5 + 3}), 0u);
    error(ServiceKind::Lseek, {A, Unit * 2, 4}, 6);
    for (auto FD : {A, B, D})
      EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    const auto Reopened = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Lseek, {Reopened, 0, 4}), Unit);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Reopened, Unit, 3}), Unit * 2);
  }
}

TEST_P(DarwinFileTest,
       SparseSeekRequiresKnownAllocationAndPreservesErrorState) {
  const auto Unknown = ok(ServiceKind::Open, {Base});
  for (uint64_t Whence : {3u, 4u}) {
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {Unknown, 0, Whence}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
    error(ServiceKind::Lseek, {999, UINT64_MAX, Whence}, 9);
    error(ServiceKind::Lseek, {1, UINT64_MAX, Whence}, 29);
  }
  mutationPolicy();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 4}), 2u);
  error(ServiceKind::Write, {FD, UINT64_MAX, 1}, 14);
  path("X", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 0}), 1u);
  for (uint64_t Whence : {3u, 4u}) {
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {FD, 0, Whence}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 2u);
  }
  path("/");
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Directory, 0, 3}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
}

TEST_P(DarwinFileTest, SparseSeekEmptyInputAndRejectedGrowthKeepTheirState) {
  mutationPolicy();
  Options->Files["/data"].clear();
  Options->Metadata["/data"].Size = Options->Metadata["/data"].Blocks = 0;
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  for (uint64_t Whence : {3u, 4u})
    error(ServiceKind::Lseek, {FD, 0, Whence}, 6);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 8193}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {FD, darwin_file_limits::Bytes}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 8192, 3}), 8192u);
  error(ServiceKind::Lseek, {FD, 8192, 4}, 6);
  error(ServiceKind::Lseek, {FD, 8193, 3}, 6);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 8192u);
}

TEST_P(DarwinFileTest, MutationMetadataSharesOneNodeAndPreservesOtherFields) {
  mutationPolicy();
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  auto Expected = status(B);
  const auto Initial = Expected;
  EXPECT_EQ(ok(ServiceKind::Write, {A, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(status(B), Initial);
  // A same-size truncate still publishes the explicitly configured times.
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 6}), 0u);
  for (auto Offset : {48u, 64u}) {
    llvm::support::endian::write64le(Expected.data() + Offset, uint64_t(-7));
    llvm::support::endian::write64le(Expected.data() + Offset + 8, 123456789);
  }
  EXPECT_EQ(status(A), Expected);
  EXPECT_EQ(status(B), Expected);
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {D, Base + 128, 2, 8191}), 2u);
  llvm::support::endian::write64le(Expected.data() + 96, 8193);
  llvm::support::endian::write64le(Expected.data() + 104, 24);
  EXPECT_EQ(status(B), Expected);
  for (auto Kind : {ServiceKind::Stat64, ServiceKind::Lstat64}) {
    EXPECT_EQ(ok(Kind, {Base, Base + 256}), 0u);
    std::array<uint8_t, 144> Bytes;
    llvm::cantFail(Space->read(Base + 256, Bytes));
    EXPECT_EQ(Bytes, Expected);
  }
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {B, UINT64_MAX, Base + 256, 0x400}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
  for (auto FD : {A, B, D})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  EXPECT_EQ(status(ok(ServiceKind::Open, {Base})), Expected);
  EXPECT_EQ(Options->Metadata.at("/data").Size, 6u);
  EXPECT_EQ(Options->Metadata.at("/data").Blocks, 8u);
  EXPECT_EQ(Options->Metadata.at("/data").ModificationTime.Seconds, INT64_MAX);
}

TEST_P(DarwinFileTest, SparseUnitMetadataTracksHolesWritesAndDiscardedUnits) {
  for (uint32_t Unit : {512u, 4096u, 16384u}) {
    SCOPED_TRACE(Unit);
    mutationPolicy(Unit);
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto FD = ok(ServiceKind::Open, {Base, 2});
    auto Check = [&](uint64_t Size, uint64_t Blocks) {
      auto Bytes = status(FD);
      EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 96), Size);
      EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 104), Blocks);
      // I/O advice stays independent of the policy's allocation unit.
      EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 112), 4096u);
    };
    Check(6, Unit / 512);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit * 6 + 1}), 0u);
    Check(Unit * 6 + 1, Unit / 512);
    llvm::cantFail(Space->writeInteger(Base + 128, 0, 2));
    // A zero-valued write into a hole allocates, even without a byte change.
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 2, Unit * 2 - 1}), 2u);
    Check(Unit * 6 + 1, Unit / 512 * 3);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 2, Unit * 2 - 1}), 2u);
    Check(Unit * 6 + 1, Unit / 512 * 3);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit + 1}), 0u);
    Check(Unit + 1, Unit / 512 * 2);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit * 6 + 1}), 0u);
    Check(Unit * 6 + 1, Unit / 512 * 2);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit}), 0u);
    Check(Unit, Unit / 512);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 0}), 0u);
    Check(0, 0);
    auto Empty = ok(ServiceKind::Open, {Base, 0x400});
    EXPECT_EQ(status(Empty), status(FD));
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit * 6 + 1}), 0u);
    Check(Unit * 6 + 1, 0);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, Unit * 4}), 1u);
    Check(Unit * 6 + 1, Unit / 512);
  }
}

TEST_P(DarwinFileTest, MutationMetadataRetainsKnownStateAcrossRejectedEffects) {
  mutationPolicy();
  const auto FD = ok(ServiceKind::Open, {Base, 10});
  const auto Initial = status(FD);
  const uint64_t End = Base + Page * 2 - 2;
  path("X", End);
  EXPECT_FALSE(invoke(ServiceKind::Write, {FD, End, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialWrite);
  EXPECT_EQ(status(FD), Initial);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {FD, darwin_file_limits::Bytes}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(status(FD), Initial);
  {
    auto Mapping = Files->mappingSource(FD);
    EXPECT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Mapping));
    EXPECT_FALSE(invoke(ServiceKind::Write, {FD, End, 1}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
    EXPECT_EQ(status(FD), Initial);
  }
  EXPECT_EQ(ok(ServiceKind::Write, {FD, End, 1}), 1u);
  const auto Current = status(FD);
  EXPECT_NE(Current, Initial);
  error(ServiceKind::Fstat64, {FD, 0}, 14);
  const uint64_t Partial = Base + Page * 2 - 8;
  llvm::cantFail(Space->writeInteger(Partial, 0xaabbccddeeff0011, 8));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Partial}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialStatus);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Partial, 8)),
            0xaabbccddeeff0011u);
  EXPECT_EQ(status(FD), Current);
}

TEST_P(DarwinFileTest, InitialDenseAllocationIncludesEmptyAndMultipleUnits) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    mutationPolicy(512);
    const unsigned Size = Case ? 513 : 0;
    Options->Files["/data"] = std::vector<uint8_t>(Size, 0);
    Options->Metadata["/data"].Size = Size;
    Options->Metadata["/data"].Blocks = Case ? 2 : 0;
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto FD = ok(ServiceKind::Open, {Base, Case ? 2u : 0x402u});
    path("", Base + 128);
    if (Case == 1)
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 512}), 0u);
    else if (Case == 2)
      EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 0}), 1u);
    else
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 0}), 0u);
    EXPECT_EQ(llvm::support::endian::read64le(status(FD).data() + 104), Case);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 2048}), 0u);
    EXPECT_EQ(llvm::support::endian::read64le(status(FD).data() + 104), Case);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 1536}), 1u);
    EXPECT_EQ(llvm::support::endian::read64le(status(FD).data() + 104),
              Case + 1);
  }
}

TEST_P(DarwinFileTest,
       UnknownMutationMetadataCannotBeResurrectedByLaterSuccess) {
  for (bool InitiallyModified : {false, true}) {
    mutationPolicy();
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto FD = ok(ServiceKind::Open, {Base, 10});
    if (InitiallyModified)
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 8}), 0u);
    error(ServiceKind::Write, {FD, UINT64_MAX, 1}, 14);
    path("X", Base + 128);
    EXPECT_EQ(ok(ServiceKind::Write, {FD, Base + 128, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    const auto Reopened = ok(ServiceKind::Open, {Base});
    contents(Reopened, {'a'});
    llvm::cantFail(Space->writeInteger(Base + 256, 0xaabb, 2));
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Reopened, Base + 256}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0xaabbu);
    EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + 256}));
  }
}

TEST(DarwinFileOptions, CreationPolicyRequiresExplicitParentsAndFreshIdentity) {
  DarwinFileOptions Good;
  Good.MutableDirectories.insert("/");
  Good.Metadata["/"] = darwin_test::creationParentMetadata();
  Good.InitialUmask = 07777;
  Good.CreationPolicy = darwin_test::CreationPolicy;
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (unsigned Case = 0; Case != 14; ++Case) {
    SCOPED_TRACE(Case);
    auto O = Good;
    auto &P = *O.CreationPolicy;
    switch (Case) {
    case 0:
      O.MutableDirectories.clear();
      break;
    case 1:
      O.InitialUmask.reset();
      break;
    case 2:
      O.InitialUmask = 010000;
      break;
    case 3:
      O.Metadata.clear();
      break;
    case 4:
      P.FirstInode = 0;
      break;
    case 5:
      P.FirstInode = 41;
      break;
    case 6:
      P.BlockSize = 0;
      break;
    case 7:
      P.BlockSize = uint32_t(INT32_MAX) + 1;
      break;
    case 8:
      P.Time.Nanoseconds = -1;
      break;
    case 9:
      P.Time.Nanoseconds = 1000000000;
      break;
    case 10:
      P.Mutation.AllocationUnit = 513;
      break;
    case 11:
      P.Mutation.Time.Nanoseconds = -1;
      break;
    case 12:
      O.Directories.insert("/empty");
      O.MutableDirectories.insert("/empty");
      break;
    case 13:
      O.Files["/data"] = {};
      O.Directories.insert("/empty");
      O.DirectoryContents["/"] = darwin_test::directoryContents();
      P.FirstInode = 0xfedcba9876543210ULL;
      break;
    }
    auto Rejected = validateFileOptions(O);
    EXPECT_TRUE(bool(Rejected));
    llvm::consumeError(std::move(Rejected));
  }
  Good.CreationPolicy->FirstInode = UINT64_MAX;
  Good.CreationPolicy->Time = {INT64_MIN, 999999999};
  Good.CreationPolicy->Mutation.Time = {INT64_MAX, 0};
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  Good.Metadata["/"].Inode = UINT64_MAX;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileCreationPolicy);
}

TEST(DarwinFileOptions, MutationPolicyAdmitsOnlyExplicitCoherentMetadata) {
  DarwinFileOptions Good;
  Good.Files["/data"] = std::vector<uint8_t>(10);
  Good.WritableFiles.insert("/data");
  Good.Metadata["/data"] = darwin_test::mutationMetadata();
  Good.MutationPolicies["/data"] = darwin_test::MutationPolicy;
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (uint32_t Unit :
       {512u, 4096u, 16384u, uint32_t(darwin_file_limits::Bytes)}) {
    auto O = Good;
    O.MutationPolicies["/data"].AllocationUnit = Unit;
    O.Metadata["/data"].Blocks = Unit / 512;
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  for (unsigned Case = 0; Case != 13; ++Case) {
    auto O = Good;
    auto &P = O.MutationPolicies["/data"];
    auto &M = O.Metadata["/data"];
    switch (Case) {
    case 0:
      O.WritableFiles.clear();
      break;
    case 1:
      O.Metadata.clear();
      break;
    case 2:
      P.AllocationUnit = 0;
      break;
    case 3:
      P.AllocationUnit = 256;
      break;
    case 4:
      P.AllocationUnit = 513;
      break;
    case 5:
      P.AllocationUnit = darwin_file_limits::Bytes * 2;
      break;
    case 6:
      P.Time.Nanoseconds = -1;
      break;
    case 7:
      P.Time.Nanoseconds = 1000000000;
      break;
    case 8:
      M.Blocks = 0;
      break;
    case 9:
      M.Mode |= 04000;
      break;
    case 10:
      M.Mode |= 02000;
      break;
    case 11:
      M.Mode |= 01000;
      break;
    case 12:
      M.LinkCount = 2;
      break;
    }
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileMutationPolicy)
        << Case;
  }
  Good.Metadata["/data"].Flags = 1;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileMutationPolicy);
  Good.Metadata["/data"].Flags = 0;
  Good.MutationPolicies["/absent"] = darwin_test::MutationPolicy;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileMutationPolicy);
  Good.MutationPolicies.erase("/absent");
  Good.StandardInput = std::vector<uint8_t>(darwin_file_limits::Bytes - 28);
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  Good.StandardInput->push_back(0);
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, WriteErrorOrderZeroCountsAndClippedAppendMatchNative) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto RO = ok(ServiceKind::Open, {Base});
  auto WO = ok(ServiceKind::Open, {Base, 1});
  error(ServiceKind::Write, {999, 0, 0x80000000}, 22);
  error(ServiceKind::Write, {999, 0, 0}, 9);
  error(ServiceKind::Pwrite, {999, 0, 0, UINT64_MAX}, 22);
  error(ServiceKind::Pwrite, {999, 0, 0, UINT64_MAX - 1}, 9);
  error(ServiceKind::Pwrite, {A, 0, 0, UINT64_MAX - 1}, 22);
  error(ServiceKind::Pwrite, {1, 0, 0, 0}, 29);
  error(ServiceKind::Write, {RO, 0, 0}, 9);
  error(ServiceKind::Read, {WO, 0, 0}, 9);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, UINT64_MAX, 0, 99}), 0u);
  error(ServiceKind::Pwrite, {A, 0, 0, INT64_MAX}, 27);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 4, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Write, {A, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
  error(ServiceKind::Write, {A, UINT64_MAX, 2}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 6u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Write, {A, 0, 0}, 27);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, INT64_MAX - 1, 0}),
            uint64_t(INT64_MAX - 1));
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + 128, 2}), 1u);
  contents(RO, {'a', 'b', 0, 0xff, 'e', 'f', 'X'});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 7u);
}

TEST_P(DarwinFileTest, PartialInputRefusesEffectsAndDirtyMetadataIsNotReused) {
  Options->WritableFiles.insert("/data");
  auto M = darwin_test::metadata(6);
  M.Flags = 0;
  Options->Metadata["/data"] = M;
  auto A = ok(ServiceKind::Open, {Base, 10});
  auto B = ok(ServiceKind::Open, {Base});
  const uint64_t End = Base + Page * 2 - 2;
  path("X", End);
  EXPECT_FALSE(invoke(ServiceKind::Write, {A, End, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialWrite);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
  contents(B, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Fstat64, {A, Base + 256}), 0u);
  EXPECT_EQ(ok(ServiceKind::Write, {A, End, 1}), 1u);
  ASSERT_FALSE(bool(Space->writeInteger(Base + 256, 0x7777, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0x7777u);
  ok(ServiceKind::Close, {A});
  ok(ServiceKind::Close, {B});
  auto C = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {C, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + 256}));
}

TEST_P(DarwinFileTest, FailedAppendInvalidatesMetadataWithoutPublishingBytes) {
  Options->WritableFiles.insert("/data");
  auto M = darwin_test::metadata(6);
  M.Flags = 0;
  Options->Metadata["/data"] = M;
  const auto A = ok(ServiceKind::Open, {Base, 10});
  const auto B = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Write, {A, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fstat64, {B, Base + 256}), 0u);
  error(ServiceKind::Write, {A, UINT64_MAX, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 6u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 10u);
  contents(B, {'a', 'b', 0, 0xff, 'e', 'f'});
  llvm::cantFail(Space->writeInteger(Base + 256, 0x7777, 2));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0x7777u);
}

TEST_P(DarwinFileTest, AggregateMutationBudgetIsReclaimedByShrinking) {
  Options->Files["/other"] = {};
  Options->WritableFiles = {"/data", "/other"};
  Options->StandardInput = {1, 2, 3};
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 3;
  auto A = ok(ServiceKind::Open, {Base, 2});
  path("/other", Base + 128);
  auto B = ok(ServiceKind::Open, {Base + 128, 2});
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Pwrite, {A, Base, 1, INT64_MAX - 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 2}), 0u);
}

TEST(DarwinFileOptions, WritableAdmissionRejectsAliasesAndRestrictiveFlags) {
  DarwinFileOptions O;
  O.Files["/a"] = {};
  O.Files["/b"] = {};
  O.WritableFiles = {"/missing"};
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileWritableOption);
  O.WritableFiles = {"/a"};
  auto M = darwin_test::metadata(0);
  M.Flags = 0;
  O.Metadata["/a"] = O.Metadata["/b"] = M;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileWritableAlias);
  O.Metadata["/b"].Device++;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  for (uint32_t Flags : {2u, 4u, 0x20000u, 0x40000u}) {
    O.Metadata["/a"].Flags = Flags;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileWritableFlags);
  }
}

class FailingFileInput : public GuestMemory {
  GuestMemory &Memory;

public:
  bool FailAccess = false, FailRead = false;
  uint64_t FailureAddress = 0;
  explicit FailingFileInput(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error map(uint64_t A, uint64_t S, unsigned P) override {
    return Memory.map(A, S, P);
  }
  llvm::Error protect(uint64_t A, uint64_t S, unsigned P) override {
    return Memory.protect(A, S, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (FailRead && A >= FailureAddress) {
      // A transport may fill part of the scratch span before failing.
      B.front() = 'x';
      return failure("file input transport failed");
    }
    return Memory.read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    return Memory.write(A, B);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t S,
                                 unsigned P) const override {
    if (FailAccess && A >= FailureAddress)
      return failure("file input preflight failed");
    return Memory.canAccess(A, S, P);
  }
};

TEST_P(DarwinFileTest, DirectoryPathTransportFailuresKeepNamespaceUnchanged) {
  Options->MutableDirectories.insert("/");
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  path("/new");
  for (auto Kind : {ServiceKind::Mkdir, ServiceKind::Rmdir}) {
    for (bool Preflight : {true, false}) {
      Input.FailureAddress = Base + 2;
      Input.FailAccess = Preflight;
      Input.FailRead = !Preflight;
      auto Failed = Files->handle(Kind, {0, 0, {Base}, std::nullopt}, Result);
      ASSERT_FALSE(bool(Failed));
      llvm::consumeError(Failed.takeError());
      Input.FailAccess = Input.FailRead = false;
      if (Kind == ServiceKind::Mkdir)
        error(ServiceKind::Access, {Base}, 2);
      else
        EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
    }
    EXPECT_EQ(ok(Kind, {Base}), 0u);
  }
  const auto FD = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(FD, 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, AccessTransportFailuresPreserveFilesAndDescriptors) {
  mutationPolicy();
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base});
  const auto Before = status(FD);
  path("/data", Base + 128);
  for (bool Preflight : {true, false}) {
    Input.FailureAddress = Base + 130;
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Access,
                                {0, 33, {Base + 128}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    llvm::consumeError(Failed.takeError());
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0u);
  }
}

TEST_P(DarwinFileTest, RenameTargetInputFailuresPreserveBothObjects) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto Before = status(A);
  path("/target", Base + 128);
  const auto T = ok(ServiceKind::Open, {Base + 128});
  for (bool Preflight : {true, false}) {
    Input.FailureAddress = Base + 128;
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed =
        Files->handle(ServiceKind::Rename,
                      {0, 128, {Base, Base + 128}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    identity(A, "/data");
    identity(T, "/target");
    EXPECT_EQ(status(A), Before);
    contents(T, {'T'});
  }
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 14);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  identity(A, "/target");
  contents(T, {'T'});
}

TEST_P(DarwinFileTest, UmaskNeedsNeitherGuestMemoryNorAvailableDescriptors) {
  Options->InitialUmask = 0027;
  Options->DescriptorLimit = 3;
  FailingFileInput Input(*Space);
  Input.FailAccess = Input.FailRead = true;
  Files = std::make_unique<DarwinFiles>(Input, Options);
  EXPECT_EQ(ok(ServiceKind::Umask, {07000, UINT64_MAX}), 0027u);
  error(ServiceKind::Open, {UINT64_MAX}, 24);
  EXPECT_EQ(ok(ServiceKind::Umask, {0022, UINT64_MAX}), 07000u);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest, CreatePathFailuresPreserveMissingNameAndDescriptor) {
  Options->MutableDirectories.insert("/");
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  path("/new");
  for (bool Preflight : {true, false}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Open,
                                {0, 5, {Base, 0xa02}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    error(ServiceKind::Open, {Base}, 2);
  }
  const uint64_t End = Base + Page * 2;
  llvm::cantFail(Space->writeInteger(End - 1, '/', 1));
  error(ServiceKind::Open, {End - 1, 0xa02}, 14);
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0xa02}), 3u);
}

TEST_P(DarwinFileTest, UnlinkPathFailuresPreserveNameAndMetadata) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  const auto Initial = status(FD);
  for (bool Preflight : {true, false}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Unlink,
                                {0, 10, {Base}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(status(FD), Initial);
    const auto B = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  }
  const uint64_t End = Base + Page * 2;
  llvm::cantFail(Space->writeInteger(End - 1, '/', 1));
  error(ServiceKind::Unlink, {End - 1}, 14);
  EXPECT_EQ(status(FD), Initial);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
}

TEST_P(DarwinFileTest,
       InputTransportFailuresDoNotCommitContentsOrAppendCursor) {
  mutationPolicy();
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base, 10});
  const auto Initial = status(FD);
  for (bool Preflight : {true, false}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Write,
                                {0, 4, {FD, Base, 2}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
    contents(FD, {'a', 'b', 0, 0xff, 'e', 'f'});
    EXPECT_EQ(status(FD), Initial);
  }
}

TEST_P(DarwinFileTest, DupSharesOffsetsButOpenAndPreadDoNot) {
  auto A = ok(ServiceKind::Open, {Base});
  auto B = ok(ServiceKind::Dup, {A});
  auto C = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {A, Base + Page, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {C, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Pread, {B, Base + Page, 2, 2}), 2u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 2)), 0xff00u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {B, Base + Page, 1}), 1u);
  error(ServiceKind::Read, {A, Base + Page, 1}, 9);
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), A);
}

TEST_P(DarwinFileTest, PerDescriptorFlagsDup2AndCaptureSinkIdentity) {
  auto A = ok(ServiceKind::Open, {Base, 0x1000000});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, A}), A);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  auto B = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 2, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 2, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 67, 20}), 20u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {20, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 0, 20}), 21u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {21, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {1, A}), A);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {1}), 0u);
  error(ServiceKind::Write, {1, Base, 1}, 9);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Write, {2, Base, 1}), 1u);
  EXPECT_EQ(Result.StandardOutput, "///");
  EXPECT_TRUE(Result.StandardError.empty());
  error(ServiceKind::Dup2, {A, UINT64_MAX}, 9);
  error(ServiceKind::Fcntl, {A, 0, UINT64_MAX}, 22);
}

TEST_P(DarwinFileTest, ReadErrorsEOFAndSeekOverflowPreserveCursor) {
  auto FD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {99, 0, 0x80000000}, 22);
  error(ServiceKind::Pread, {99, 0, UINT64_MAX, UINT64_MAX}, 22);
  error(ServiceKind::Read, {99, 0, 1}, 9);
  error(ServiceKind::Read, {FD, 0, 1}, 14);
  error(ServiceKind::Read, {FD, UINT64_MAX, 1}, 14);
  error(ServiceKind::Pread, {FD, Base + Page, 0, UINT64_MAX}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 1, 2}), 7u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {FD, 1, 1}, 84);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {FD, uint64_t(INT64_MIN), 1}, 22);
  error(ServiceKind::Lseek, {FD, 0, 99}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, uint64_t(-1), 2}), 5u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, Base + Page, 10}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'f');
}

TEST_P(DarwinFileTest, PartialCopyoutIsRefusedBeforeBytesOrOffsetChange) {
  auto FD = ok(ServiceKind::Open, {Base});
  const uint64_t End = Base + Page * 2 - 2;
  ASSERT_FALSE(bool(Space->writeInteger(End, 0x7777, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Read, {FD, End, 4}));
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 2)), 0x7777u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  error(ServiceKind::Read, {FD, Base + Page, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, FiniteInputSharesItsCursorAndClosedFDZeroIsReusable) {
  EXPECT_FALSE(invoke(ServiceKind::Read, {0, Base + Page, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInput);
  Options->StandardInput = {0, 0xff, 'x'};
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  auto Copy = ok(ServiceKind::Dup, {0});
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, Base + Page, 2}), 2u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 2)), 0xff00u);
  error(ServiceKind::Pread, {Copy, Base + Page, 1, 0}, 29);
  error(ServiceKind::Lseek, {Copy, 0, 0}, 29);
  EXPECT_EQ(ok(ServiceKind::Close, {0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, Base + Page, 3}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'x');
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, 0, 1}), 0u);
  Options->StandardInput = std::vector<uint8_t>();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_EQ(ok(ServiceKind::Read, {0, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, PathErrorsAndResourceExhaustionDoNotLeakDescriptors) {
  Options->Files["/nested/file"] = {'x'};
  path("/nested/file");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  path("/nested");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  for (unsigned I = 0; I != 4; ++I)
    error(ServiceKind::Open, {0}, 14);
  path("");
  error(ServiceKind::Open, {Base}, 2);
  path("/missing");
  error(ServiceKind::Open, {Base}, 2);
  path("/data/child");
  error(ServiceKind::Open, {Base}, 20);
  path("/data/" + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 20);
  path("/missing/" + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 2);
  path('/' + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 63);
  path(std::string(1024, 'a'));
  error(ServiceKind::Open, {Base}, 63);
  path("/data", Base + Page * 2 - 6);
  EXPECT_EQ(ok(ServiceKind::Open, {Base + Page * 2 - 6}), 3u);
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {0}, 24);
  error(ServiceKind::Dup, {3}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
}

TEST_P(DarwinFileTest, UnsupportedOperationsDoNotPretendToBeMissingFiles) {
  for (auto Text : {"relative", "./data", "../data"}) {
    path(Text);
    EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  }
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 1}));
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {3, 999}));
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {3, 0, 3}));
  Options.reset();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  Options.emplace();
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, Stat64PreservesAllFieldsPaddingAndDescriptorOffsets) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  // Independently specified little-endian LP64 record, including the rdev,
  // alignment gap and all reserved bytes. Also checked against the native SDK.
  const auto Expected = llvm::fromHex(
      "85ffffffa48103001032547698badcfeefcdab8998badcfe0000000000000000"
      "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
      "fdffffffffffffff0400000000000000fbffffffffffffff0600000000000000"
      "060000000000000008000000000000000010000034120000efcdab8900000000"
      "00000000000000000000000000000000");
  const uint64_t Buffer = Base + Page - 7;
  auto FD = ok(ServiceKind::Open, {Base});
  auto Copy = ok(ServiceKind::Dup, {FD});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    ASSERT_FALSE(bool(Space->writeInteger(Buffer - 1, 0xaa, 1)));
    ASSERT_FALSE(bool(Space->writeInteger(Buffer + 144, 0xbb, 1)));
    EXPECT_EQ(ok(Kind, {Kind == ServiceKind::Fstat64 ? Copy : Base, Buffer}),
              0u);
    std::array<uint8_t, 144> Bytes;
    ASSERT_FALSE(bool(Space->read(Buffer, Bytes)));
    EXPECT_EQ(llvm::toHex(llvm::ArrayRef<uint8_t>(Bytes)),
              llvm::toHex(Expected));
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer - 1, 1)), 0xaau);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 144, 1)), 0xbbu);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Copy, 0, 1}), 2u);
  }
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), FD);
}

TEST_P(DarwinFileTest, Stat64FailureOrderAndPartialOutputHaveNoSideEffects) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  auto FD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Fstat64, {99, 0}, 9);
  error(ServiceKind::Stat64, {0, 0}, 14);
  path("/absent");
  error(ServiceKind::Stat64, {Base, 0}, 2);
  path("/data/child");
  error(ServiceKind::Lstat64, {Base, 0}, 20);
  path("/data");
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {Base}, 24);
  EXPECT_EQ(ok(ServiceKind::Stat64, {Base, Base + Page}), 0u);
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    const uint64_t Source = Kind == ServiceKind::Fstat64 ? FD : Base;
    error(Kind, {Source, 0}, 14);
    error(Kind, {Source, UINT64_MAX}, 14);
    const auto End = Base + Page * 2 - 8;
    ASSERT_FALSE(bool(Space->writeInteger(End, 0xaabbccddeeff0011, 8)));
    EXPECT_FALSE(invoke(Kind, {Source, End}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialStatus);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 8)), 0xaabbccddeeff0011u);
  }
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  error(ServiceKind::Fstat64, {FD, Base + Page}, 14);
}

TEST_P(DarwinFileTest, UnknownMetadataAndDirectoriesAreNotFabricated) {
  auto FD = ok(ServiceKind::Open, {Base});
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    EXPECT_FALSE(
        invoke(Kind, {Kind == ServiceKind::Fstat64 ? FD : Base, Base + Page}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  }
  for (auto FD : {0u, 1u, 2u}) {
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + Page}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  }
  path("/");
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  Options.reset();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Lstat64, {Base, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
}

TEST(DarwinFileOptions,
     MetadataAdmissionRejectsUnknownPathsAndIncoherentValues) {
  DarwinFileOptions O;
  O.Files["/data"] = std::vector<uint8_t>(10);
  O.Metadata["/missing"] = darwin_test::metadata();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataPath);
  O.Metadata.clear();
  auto &M = O.Metadata["/data"];
  for (unsigned Case = 0; Case != 8; ++Case) {
    M = darwin_test::metadata();
    switch (Case) {
    case 0:
      M.Mode = 0040644;
      break;
    case 1:
      M.Size = 9;
      break;
    case 2:
      M.BlockSize = uint32_t(INT32_MAX) + 1;
      break;
    case 3:
      M.Blocks = uint64_t(INT64_MAX) + 1;
      break;
    case 4:
      M.AccessTime.Nanoseconds = -1;
      break;
    case 5:
      M.ModificationTime.Nanoseconds = 1000000000;
      break;
    case 6:
      M.ChangeTime.Nanoseconds = -1;
      break;
    case 7:
      M.BirthTime.Nanoseconds = 1000000000;
      break;
    }
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileMetadataOption);
  }
  M = darwin_test::metadata();
  ASSERT_FALSE(bool(validateFileOptions(O)));
}

TEST(DarwinFileOptions, DirectoryAdmissionIncludesImplicitPathsAndCWD) {
  DarwinFileOptions O;
  O.Files["/tree/data"] = {};
  O.Directories = {"/tree", "/tree/empty/deep"};
  O.WorkingDirectory = "/tree/empty";
  auto M = darwin_test::metadata(128);
  M.Mode = 0040755;
  O.Metadata["/"] = M;
  O.Metadata["/tree/empty"] = M;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  for (auto Bad : {"", "relative", "/tree/data", "/absent", "/tree/../tree"}) {
    O.WorkingDirectory = Bad;
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << Bad;
    llvm::consumeError(std::move(E));
  }
  O.WorkingDirectory = "/";
  for (auto Bad : {"/tree/data", "/tree/data/child", "/tree/", "relative"}) {
    O.Directories.insert(Bad);
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileOptionPath);
    O.Directories.erase(Bad);
  }
  O.Metadata["/"].Size = uint64_t(INT64_MAX) + 1;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataOption);
  O.Metadata["/"] = darwin_test::metadata();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataOption);
  O = {};
  for (unsigned I = 0; I != 255; ++I)
    O.Directories.insert("/dir" + std::to_string(I));
  O.Files["/data"] = {};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/extra");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  O.WorkingDirectory = "/"; // Six path bytes plus two CWD bytes fit exactly.
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, RelativeLookupWalksAncestorsAndPreservesErrorOrder) {
  Options->Directories.insert("/empty");
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base, value::OpenDirectory});
  auto Dup = ok(ServiceKind::Dup, {Dir});
  path("/data");
  auto File = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Open, {Base, value::OpenDirectory}, 20);
  for (auto Text : {"/data/.", "/data/..", "/data/", "/data//child"}) {
    path(Text);
    error(ServiceKind::Open, {Base}, 20);
  }
  path("/absent/../data");
  error(ServiceKind::Open, {Base}, 2);
  for (auto Text : {"//./data", "/empty/../../data", "/../data"}) {
    path(Text);
    auto FD = ok(ServiceKind::OpenAt, {999, Base});
    EXPECT_EQ(ok(ServiceKind::Read, {FD, Base + Page, 1}), 1u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
    ok(ServiceKind::Close, {FD});
  }
  path("../data");
  auto FD = ok(ServiceKind::OpenAt, {Dup, Base});
  ok(ServiceKind::Close, {FD});
  error(ServiceKind::OpenAt, {999, Base}, 9);
  error(ServiceKind::OpenAt, {File, Base}, 20);
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {0, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  path("");
  error(ServiceKind::OpenAt, {999, Base}, 9);
  error(ServiceKind::OpenAt, {File, Base}, 20);
  error(ServiceKind::OpenAt, {Dir, Base}, 2);
  error(ServiceKind::Open, {Base}, 2);
  error(ServiceKind::OpenAt, {999, 0}, 14);
  Options->DescriptorLimit = 6;
  error(ServiceKind::OpenAt, {999, 0}, 24);
}

TEST_P(DarwinFileTest,
       WorkingDirectorySurvivesFailuresClosureAndFDReplacement) {
  Options->Files["/tree/data"] = {'t'};
  Options->Directories.insert("/empty");
  Options->WorkingDirectory = "/tree";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("data");
  auto File = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {File, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 't');
  error(ServiceKind::Fchdir, {File}, 20);
  error(ServiceKind::Fchdir, {999}, 9);
  path("/missing");
  error(ServiceKind::Chdir, {Base}, 2);
  path("/data");
  error(ServiceKind::Chdir, {Base}, 20);
  path(".");
  auto Dir = ok(ServiceKind::Open, {Base});
  ok(ServiceKind::Fchdir, {Dir});
  ok(ServiceKind::Close, {Dir});
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), Dir);
  path("data");
  auto StillTree = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {StillTree, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 't');
  path("/empty");
  auto Empty = ok(ServiceKind::Open, {Base});
  ok(ServiceKind::Fchdir, {Empty});
  ok(ServiceKind::Dup2, {File, Empty});
  path("data");
  error(ServiceKind::Open, {Base}, 2);
  path("..");
  ok(ServiceKind::Chdir, {Base});
  path("data");
  auto RootFile = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {RootFile, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
}

TEST_P(DarwinFileTest, DirectoryMetadataReadAndSeekUseExplicitObservations) {
  Options->Directories.insert("/empty");
  auto M = darwin_test::metadata(128);
  M.Mode = 0040700;
  Options->Metadata["/empty"] = M;
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base, value::OpenDirectory});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Dir, value::GetFileFlags}), 0u);
  for (auto Kind : {ServiceKind::Read, ServiceKind::Pread})
    for (unsigned Count : {0, 1})
      error(Kind, {Dir, 0, Count}, 21);
  error(ServiceKind::Pread, {Dir, 0, 0, UINT64_MAX}, 22);
  EXPECT_EQ(std::get<uint32_t>(Files->mappingSource(Dir)), 22u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 7, 0}), 7u);
  auto Dup = ok(ServiceKind::Dup, {Dir});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 2}), 128u);
  error(ServiceKind::Lseek, {Dir, UINT64_MAX, 0}, 22);
  error(ServiceKind::Lseek, {Dir, uint64_t(-129), 1}, 22);
  error(ServiceKind::Lseek, {Dir, 0, 99}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {Dir, 1, 1}, 84);
  for (unsigned Whence : {3, 4})
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {Dir, 0, Whence}));
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 1}), uint64_t(INT64_MAX));
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dir, 0, Base + Page, value::AtFDOnly}),
            0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 4, 2)), 0040700u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 96, 8)), 128u);
  path("/");
  auto Root = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 7, 0}), 7u);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Root, 0, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), 7u);
}

TEST_P(DarwinFileTest,
       FstatAtSharesPathOwnerAndGetPathCopiesCanonicalIdentity) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  Options->Directories.insert("/empty");
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base});
  path("..//./data");
  auto File = ok(ServiceKind::OpenAt, {Dir, Base});
  auto Dup = ok(ServiceKind::Dup, {File});
  ok(ServiceKind::Close, {File});
  const auto Buffer = Base + Page;
  for (auto Flags : {0u, 0x20u, 0x800u, 0x820u}) {
    EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dir, Base, Buffer, Flags}), 0u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 96, 8)), 6u);
  }
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dup, 0, Buffer, 0x400}), 0u);
  error(ServiceKind::FstatAt64, {999, 0, 0, 1}, 22);
  error(ServiceKind::FstatAt64, {999, 0, 0}, 14);
  error(ServiceKind::FstatAt64, {999, 0, 0, 0x400}, 9);
  EXPECT_FALSE(invoke(ServiceKind::FstatAt64, {Dir, Base, Buffer, 0x200}));
  path("");
  error(ServiceKind::FstatAt64, {999, Base, 0}, 9);
  error(ServiceKind::FstatAt64, {Dup, Base, 0}, 20);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {999, Base, Buffer}), 0u);
  ASSERT_FALSE(bool(Space->writeInteger(Buffer, UINT64_MAX, 8)));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 50, Buffer}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 0xffff00617461642fu);
  error(ServiceKind::Fcntl, {Dup, 50, 0}, 14);
  error(ServiceKind::Fcntl, {999, 50, 0}, 9);
  const auto End = Base + Page * 2 - 2;
  ASSERT_FALSE(bool(Space->writeInteger(End, 0xaaaa, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {Dup, 50, End}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialPath);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 2)), 0xaaaau);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {0, 50, Buffer}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePathIdentity);
}

TEST_P(DarwinFileTest,
       DirectoryRecordsPreserveLayoutCookiesAndIndependentOpens) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  Options->DirectoryContents["/"].Entries[2].SeekOffset = UINT64_MAX;
  path("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto B = ok(ServiceKind::Dup, {A});
  const auto C = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  error(ServiceKind::GetDirEntries64, {A, Buffer, 63, Position}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {A, Buffer, 64, Position}), 64u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 22u);
  // Independent explicit wire bytes, including native tail padding.
  const auto Expected = llvm::fromHex(
      "2900000000000000000000000000000020000100042e00000000000000000000"
      "2900000000000000000000000000000020000200042e2e000000000000000000");
  std::array<uint8_t, 64> Records;
  ASSERT_FALSE(bool(Space->read(Buffer, Records)));
  EXPECT_EQ(llvm::toHex(Records), llvm::toHex(Expected));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 32, Position}), 32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 8, 8)), UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 22u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}),
            7u); // Cookies need not increase.
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 32, Position}), 32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)),
            0xfedcba9876543210ULL);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 99u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, 0, 1, Position}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 99u);
  error(ServiceKind::GetDirEntries64, {B, 0, 0, Position}, 22);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {C, Buffer, 128, Position}), 128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  ok(ServiceKind::Lseek, {B, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 128, Position}), 128u);
  ok(ServiceKind::Lseek, {B, 88, 0});
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {B, Buffer, 128, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPosition);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 88u);
}

TEST_P(DarwinFileTest, DirectoryEOFAndExtendedFlagsRespectOriginalBufferEnd) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  path("/");
  auto FD = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  for (uint64_t Count : {1023, 1024, 4096}) {
    ok(ServiceKind::Lseek, {FD, 0, 0});
    ASSERT_FALSE(bool(Space->writeInteger(Buffer + Count - 4, 0xaaaaaaaa, 4)));
    EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, Count, Position}),
              128u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + Count - 4, 4)),
              Count < 1024 ? 0xaaaaaaaau : 1u);
  }
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, 0, 64, Position}), 0u);
  error(ServiceKind::GetDirEntries64, {FD, 0, 1024, Position}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 99u);
  // Count is capped for record payload, but its unsigned suffix address wraps.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(Buffer - 5, 0xaaaaaaaa, 4)));
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {FD, Buffer, UINT64_MAX, Position}),
      128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer - 5, 4)), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // Extended output must explicitly clear EOF when a whole-record batch is
  // shorter than the remaining snapshot. The next call resumes at its cookie.
  for (unsigned I = 0; I != 4; ++I) {
    std::string Name(255, 'x');
    Name.back() = 'a' + I;
    Options->Files['/' + Name] = {};
    Options->DirectoryContents["/"].Entries.push_back(
        {Name, 50 + I, 8, 100 + I, 0});
  }
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Position}),
            968u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 1020, 4)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 102u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Position}),
            280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 102u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 1020, 4)), 1u);
}

TEST_P(DarwinFileTest, DirectoryCopyPhasesRetainEarlierEffectsAndAliasInOrder) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  path("/");
  auto FD = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  error(ServiceKind::GetDirEntries64, {999, 0, 0, 0}, 9);
  error(ServiceKind::GetDirEntries64, {FD, 0, 128, Position}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  error(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, 0}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 41u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // A failing suffix occurs after data, offset advancement and position copy.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(Position, 0xaaaaaaaa, 8)));
  error(ServiceKind::GetDirEntries64, {FD, Buffer, Page * 4, Position}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // With overlapping outputs, position overwrites data and flags overwrite it
  // last.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 128, Buffer}), 128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 0u);
  const auto Suffix = Buffer + 1020;
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Suffix}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Suffix, 8)), 1u);
  const auto End = Base + Page * 2 - 4;
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(End, 0xbbbbbbbb, 4)));
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64, {FD, End, 128, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialData);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 4)), 0xbbbbbbbbu);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64, {FD, Buffer, 128, End}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialPosition);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 4)), 0xbbbbbbbbu);
  // Position succeeded before an individually partial extended-flag copy.
  const auto PartialCount = Base + Page * 2 - 2 - Buffer + 4;
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {FD, Buffer, PartialCount, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialFlags);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
}

TEST_P(DarwinFileTest, DirectoryLongRecordsAndMissingObservationsStayExplicit) {
  auto File = ok(ServiceKind::Open, {Base});
  error(ServiceKind::GetDirEntries64, {File, 0, 0, 0}, 22);
  path("/");
  auto Dir = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Dir, Base + 256, 1024, Base + 2048}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  Options->Directories.insert("/empty");
  const std::string Name(255, 'x');
  Options->Files['/' + Name] = {};
  auto &C = Options->DirectoryContents["/"];
  C = darwin_test::directoryContents();
  C.Entries.push_back({Name, 43, 8, 100, 0});
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dir, Base + 256, 128, Base + 2048}),
      128u);
  error(ServiceKind::GetDirEntries64, {Dir, Base + 256, 279, Base + 2048}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 1}), 99u);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dir, Base + 256, 280, Base + 2048}),
      280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 16, 2)), 280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 18, 2)), 255u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 276, 4)), 0u);
}

TEST(DarwinFileOptions,
     DirectorySnapshotAdmissionRejectsIncoherentObservations) {
  DarwinFileOptions Original;
  Original.Files["/data"] = {};
  Original.Directories.insert("/empty");
  Original.DirectoryContents["/"] = darwin_test::directoryContents();
  Original.DirectoryContents["/empty"] = {
      {{".", 42, 4, 11, 0}, {"..", 41, 4, 22, 0}}, 1};
  ASSERT_FALSE(bool(
      validateFileOptions(Original))); // Cookies are local to each directory.
  for (unsigned Case = 0; Case != 17; ++Case) {
    auto O = Original;
    auto &C = O.DirectoryContents["/"];
    switch (Case) {
    case 0:
      C.MinimumBufferSize = 0;
      break;
    case 1:
      C.MinimumBufferSize = 128 * 1024 * 1024 + 1;
      break;
    case 2:
      C.Entries[0].Inode = 0;
      break;
    case 3:
      C.Entries[0].NextOffset = 0;
      break;
    case 4:
      C.Entries[0].NextOffset = uint64_t(INT64_MAX) + 1;
      break;
    case 5:
      C.Entries[0].NextOffset = C.Entries[1].NextOffset;
      break;
    case 6:
      C.Entries[0].Type = 8;
      break;
    case 7:
      C.Entries[2].Type = 10;
      break;
    case 8:
      C.Entries[1].Name = ".";
      break;
    case 9:
      C.Entries[2].Name = "absent";
      break;
    case 10:
      C.Entries[2].Name = "empty/child";
      break;
    case 11:
      C.Entries[2].Name = std::string(256, 'a');
      break;
    case 12:
      C.Entries[2].Name = std::string("bad\0name", 8);
      break;
    case 13:
      C.Entries[0].Inode = 43;
      break; // Root . and .. name the same object.
    case 14:
      O.DirectoryContents["/empty"].Entries[0].Inode = 43;
      break;
    case 15:
      C.Entries.pop_back();
      break;
    case 16:
      C.Entries[0].MinimumBufferSize = UINT32_MAX;
      break;
    }
    SCOPED_TRACE(Case);
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryContentsOption);
  }
  auto O = Original;
  O.Metadata["/data"] = darwin_test::metadata(0);
  ASSERT_FALSE(bool(validateFileOptions(O)));
  ++O.Metadata["/data"].Inode;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryContentsOption);
  O = Original;
  O.DirectoryContents["/"].Entries[2].Type = 0;
  O.DirectoryContents["/"].Entries[2].SeekOffset = UINT64_MAX;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  for (auto Bad : {"/data", "/absent", "relative"}) {
    O.DirectoryContents[Bad] = {};
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryContentsOption);
    O.DirectoryContents.erase(Bad);
  }
}

TEST(DarwinFileOptions, DirectorySnapshotBoundsDeduplicateMetadataPaths) {
  DarwinFileOptions O;
  for (unsigned I = 0; I != 254; ++I)
    O.Directories.insert("/dir" + std::to_string(I));
  O.Files["/tree/data"] = {};
  auto M = darwin_test::metadata(128);
  M.Mode = 0040755;
  M.Inode = 41;
  O.Metadata["/tree"] = M;
  auto &C = O.DirectoryContents["/tree"];
  C = {{{".", 41, 4, 1, 0}, {"..", 40, 4, 2, 0}, {"data", 42, 8, 3, 0}}, 1};
  ASSERT_FALSE(
      bool(validateFileOptions(O))); // 255 nodes + shared implicit /tree = 256.
  O.Directories.insert("/extra");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.Directories.insert("/empty");
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 143);
  O.DirectoryContents["/"] = darwin_test::directoryContents();
  // /data NUL=6, /empty NUL=7, implicit root key=2, four records=128.
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.DirectoryContents["/"] = {};
  O.DirectoryContents["/"].MinimumBufferSize = 1;
  O.DirectoryContents["/"].Entries.resize(darwin_file_limits::DirectoryEntries +
                                          1);
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinFileTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
