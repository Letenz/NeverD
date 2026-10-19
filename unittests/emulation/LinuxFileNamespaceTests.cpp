//===- LinuxFileNamespaceTests.cpp - Independent raw FD contracts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both LP64 ABIs share exact dirent64 bytes, user-copy and open descriptions.
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
class LinuxFileNamespace : public testing::TestWithParam<GuestArchitecture> {
protected:
  std::optional<BackendSelection> Backend;
  ProcessOptions Options;
  ProcessResult Report{ProcessProfile::LinuxELF64,
                       GuestArchitecture::X64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::MemoryLayout Layout{0x800000000000ULL, 4096};
  std::unique_ptr<linux_model::LinuxServices> Kernel;
  ExecutionBackend &cpu() { return *Backend->CPU; }
  void SetUp() override {
    ExecutionConfiguration C;
    C.Backend = ExecutionBackendKind::Unicorn;
    C.Architecture = GetParam();
    C.Contract = GetParam() == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Backend.emplace(llvm::cantFail(createExecutionBackend(C, 4 * 1024 * 1024)));
    llvm::cantFail(
        cpu().addressSpace()->map(Base, 8192, Read | Write | UserAccessible));
    Options.LinuxFiles.emplace();
    Options.LinuxFiles->Files["/files/data"] = {'a', 'b', 'c'};
    Options.LinuxFiles->WriteLimits["/files/data"] = 8192;
    LinuxFileMetadata M;
    M.Mode = 0100644;
    M.Inode = 42;
    M.BlockSize = 4096;
    M.Size = 3;
    Options.LinuxFiles->Metadata["/files/data"] = M;
    Options.LinuxFiles->DynamicProcFDs = true;
    Options.LinuxFiles->Directories = {"/empty"};
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0, uint64_t D = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, D, 0, 0}, {}}));
  }
  uint64_t open(const char *Name, unsigned Flags = 0) {
    path(Name);
    return *call(ServiceKind::OpenAt, uint64_t(-100), Base, Flags);
  }
  void path(const char *Name) {
    llvm::StringRef S(Name);
    std::vector<uint8_t> V(S.begin(), S.end());
    V.push_back(0);
    llvm::cantFail(cpu().write(Base, V));
  }
  std::vector<uint8_t> bytes(uint64_t At, unsigned Count) {
    std::vector<uint8_t> V(Count);
    llvm::cantFail(cpu().read(At, V));
    return V;
  }
};

LinuxFileMetadata directorySnapshot() {
  LinuxFileMetadata M;
  M.Device = 7;
  M.Inode = 91;
  M.Mode = 0042775;
  M.UID = 1000;
  M.GID = 1234;
  M.LinkCount = 2;
  M.Size = 40;
  M.BlockSize = 4096;
  M.AccessTime = {3, 4};
  M.ModificationTime = {5, 6};
  M.ChangeTime = {7, 8};
  return M;
}
TEST_P(LinuxFileNamespace,
       CreatedDirectoriesShareInodesMetadataAndOpenCursors) {
  Options.LinuxFiles->DirectoryCreation.emplace().Parents["/empty"] =
      directorySnapshot();
  Options.LinuxTime.emplace().Clocks[0] = {100, 123};
  ASSERT_FALSE(bool(linux_model::validateFileOptions(*Options.LinuxFiles)));
  auto D = open("/empty");
  ASSERT_EQ(D, 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 48u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 0u);
  path("//empty//z///");
  ProcessServiceEvent Raw{0,
                          GetParam() == GuestArchitecture::X64 ? 258u : 34u,
                          {UINT64_MAX, Base, 0xabcdef0000001fffULL, 0, 0, 0},
                          {}};
  EXPECT_EQ(llvm::cantFail(Kernel->handle(Raw)), 0u);
  const auto Child = open("/empty/z");
  EXPECT_EQ(call(ServiceKind::Fstat, Child, Base + 1024), 0u);
  const unsigned Size = GetParam() == GuestArchitecture::X64 ? 144 : 128;
  auto S = bytes(Base + 1024, Size);
  auto Word = [&](unsigned Offset) {
    return llvm::support::endian::read64le(S.data() + Offset);
  };
  auto Int = [&](unsigned Offset) {
    return llvm::support::endian::read32le(S.data() + Offset);
  };
  const bool X64 = GetParam() == GuestArchitecture::X64;
  EXPECT_EQ(Word(0), 7u);
  const auto Inode = Word(8);
  EXPECT_NE(Inode, 91u);
  EXPECT_NE(Inode, 42u);
  EXPECT_NE(Inode, 0u);
  EXPECT_EQ(Int(X64 ? 24 : 16), 0043755u);
  EXPECT_EQ(Int(X64 ? 16 : 20), 2u);
  EXPECT_EQ(Int(X64 ? 28 : 24), 1000u);
  EXPECT_EQ(Int(X64 ? 32 : 28), 1234u);
  EXPECT_EQ(Word(48), 40u);
  EXPECT_EQ(Word(56), 4096u);
  EXPECT_EQ(Word(64), 0u);
  for (unsigned At : {72u, 88u, 104u}) {
    EXPECT_EQ(Word(At), 100u);
    EXPECT_EQ(Word(At + 8), 123u);
  }
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 24), 24u);
  auto E = bytes(Base + 512, 24);
  EXPECT_EQ(llvm::support::endian::read64le(E.data()), Inode);
  EXPECT_EQ(E[18], 4);
  EXPECT_EQ(E[19], 'z');
  EXPECT_EQ(E[20], 0);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 0u);
  // Insert a lexically earlier sibling after EOF. Existing cookies do not
  // repeat z or hide the new a; dup consumes the original description.
  ASSERT_TRUE(Kernel->clock().advanceTo(1000000007, Report));
  path("/empty/a");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0700), 0u);
  const auto Alias = call(ServiceKind::Dup, D);
  ASSERT_TRUE(Alias);
  EXPECT_EQ(call(ServiceKind::Getdents64, *Alias, Base + 512, 24), 24u);
  EXPECT_EQ(bytes(Base + 531, 2), (std::vector<uint8_t>{'a', 0}));
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 48), 0u);
  EXPECT_EQ(call(ServiceKind::Fstat, D, Base + 1024), 0u);
  S = bytes(Base + 1024, Size);
  EXPECT_EQ(Word(8), 91u);
  EXPECT_EQ(Int(X64 ? 16 : 20), 4u);
  EXPECT_EQ(Word(48), 80u);
  EXPECT_EQ(Word(72), 3u); // noatime, including enumeration
  EXPECT_EQ(Word(80), 4u);
  EXPECT_EQ(Word(88), 101u);
  EXPECT_EQ(Word(96), 130u);
  const auto ParentAfter = S;
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0), uint64_t(-17));
  EXPECT_EQ(call(ServiceKind::Fstat, D, Base + 1024), 0u);
  EXPECT_EQ(bytes(Base + 1024, Size), ParentAfter);
  EXPECT_EQ(call(ServiceKind::Close, Child), 0u);
  EXPECT_EQ(call(ServiceKind::Fstat, open("/empty/z"), Base + 1024), 0u);
  S = bytes(Base + 1024, Size);
  EXPECT_EQ(Word(8), Inode);
  path("/empty/z");
  EXPECT_EQ(call(ServiceKind::FstatAt, UINT64_MAX, Base, Base + 1280, 0), 0u);
  EXPECT_EQ(bytes(Base + 1280, Size), S);
  EXPECT_EQ(call(ServiceKind::Lseek, D, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 512, 96), 96u);
  EXPECT_EQ(bytes(Base + 579, 2), (std::vector<uint8_t>{'z', 0}));
  EXPECT_EQ(bytes(Base + 603, 2), (std::vector<uint8_t>{'a', 0}));
}
TEST_P(LinuxFileNamespace, DirectoryAdmissionIsAtomicForTimeCapacityAndMode) {
  auto &C = Options.LinuxFiles->DirectoryCreation.emplace();
  C.Parents["/empty"] = directorySnapshot();
  C.Limit = 1;
  path("/empty/new");
  EXPECT_FALSE(call(ServiceKind::MkdirAt, 0, Base, 0777));
  EXPECT_EQ(call(ServiceKind::FaccessAt, 0, Base, 0), uint64_t(-2));
  Options.LinuxTime.emplace().Clocks[0] = {100, 0};
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0500), 0u);
  path("/empty/new/sub");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0777), uint64_t(-13));
  path("/empty/second");
  EXPECT_FALSE(call(ServiceKind::MkdirAt, 0, Base, 0777));
  EXPECT_NE(Report.Diagnostic.find("reservation"), std::string::npos);
  EXPECT_EQ(call(ServiceKind::FaccessAt, 0, Base, 0), uint64_t(-2));
  path("/empty/new");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0777), uint64_t(-17));
}
TEST_P(LinuxFileNamespace, DirectoryModesUseCallerIdentityAndParentSetgid) {
  auto &C = Options.LinuxFiles->DirectoryCreation.emplace();
  auto M = directorySnapshot();
  M.Mode = 0040777;
  M.UID = 777;
  M.GID = 888;
  C.Parents["/empty"] = M;
  C.Umask = 0077;
  Options.LinuxTime.emplace().Clocks[0] = {100, 0};
  path("/empty/new");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 07777), 0u);
  const auto F = open("/empty/new");
  EXPECT_EQ(call(ServiceKind::Fstat, F, Base + 512), 0u);
  const bool X64 = GetParam() == GuestArchitecture::X64;
  auto S = bytes(Base + 512, 144);
  auto Int = [&](unsigned Offset) {
    return llvm::support::endian::read32le(S.data() + Offset);
  };
  EXPECT_EQ(Int(X64 ? 24 : 16), 0041700u);
  EXPECT_EQ(Int(X64 ? 28 : 24), 1000u);
  EXPECT_EQ(Int(X64 ? 32 : 28), 1000u);
  path("/empty/new/nested");
  EXPECT_EQ(call(ServiceKind::MkdirAt, 0, Base, 0777), 0u);
  EXPECT_EQ(call(ServiceKind::Fstat, F, Base + 512), 0u);
  S = bytes(Base + 512, 144);
  EXPECT_EQ(Int(X64 ? 16 : 20), 3u);
}

TEST_P(LinuxFileNamespace, DirectoryMetadataRejectsInodeAndNamespaceConflicts) {
  auto &C = Options.LinuxFiles->DirectoryCreation.emplace();
  C.Parents["/empty"] = directorySnapshot();
  const auto Good = *Options.LinuxFiles;
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    *Options.LinuxFiles = Good;
    auto &D = *Options.LinuxFiles->DirectoryCreation;
    if (Mode == 0)
      D.Parents["/empty"].Inode = 42;
    if (Mode == 1)
      Options.LinuxFiles->Files["/empty/child"] = {};
    if (Mode == 2)
      Options.LinuxFiles->Directories.push_back("/empty/sub");
    if (Mode == 3)
      D.Parents["/empty"].AccessTime.Nanoseconds = 1000000000;
    if (Mode == 4)
      D.Parents["/empty"].Mode = 0100755;
    if (Mode == 5)
      D.Parents["/proc/self/fd"] = directorySnapshot();
    auto E = linux_model::validateFileOptions(*Options.LinuxFiles);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}

TEST_P(LinuxFileNamespace, WorkingDirectoryHasExactBytesAndKernelErrorOrder) {
  Options.LinuxFiles->WorkingDirectory = "/empty";
  EXPECT_EQ(call(ServiceKind::Getcwd, Base + 512, 7), 7u);
  EXPECT_EQ(bytes(Base + 512, 8),
            (std::vector<uint8_t>{'/', 'e', 'm', 'p', 't', 'y', 0, 0}));
  for (uint64_t Size : {0u, 1u, 6u})
    EXPECT_EQ(call(ServiceKind::Getcwd, UINT64_MAX, Size), uint64_t(-34));
  EXPECT_EQ(call(ServiceKind::Getcwd, UINT64_MAX, 7), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Getcwd, Base + 1024, UINT64_MAX), 7u);
  EXPECT_EQ(bytes(Base + 1024, 7), bytes(Base + 512, 7));
  ProcessServiceEvent Raw{0,
                          GetParam() == GuestArchitecture::X64 ? 79u : 17u,
                          {Base + 1536, 7, 0, 0, 0, 0},
                          {}};
  EXPECT_EQ(llvm::cantFail(Kernel->handle(Raw)), 7u);
  EXPECT_EQ(bytes(Base + 1536, 7), bytes(Base + 512, 7));
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, Read | UserAccessible));
  EXPECT_FALSE(call(ServiceKind::Getcwd, Base + 4093, 7));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(bytes(Base + 4093, 3), (std::vector<uint8_t>{0, 0, 0}));
  Options.LinuxFiles->WorkingDirectory = "/";
  EXPECT_EQ(call(ServiceKind::Getcwd, Base + 512, 2), 2u);
  EXPECT_EQ(bytes(Base + 512, 2), (std::vector<uint8_t>{'/', 0}));
}

TEST_P(LinuxFileNamespace, WorkingDirectoryRequiresExplicitDirectoryIdentity) {
  EXPECT_FALSE(call(ServiceKind::Getcwd, Base + 512, 16));
  EXPECT_NE(Report.Diagnostic.find("not supplied"), std::string::npos);
  for (const char *Path : {"", "relative", "/missing", "/files/data", "/files",
                           "/empty/", "//empty", "/empty/.."}) {
    Options.LinuxFiles->WorkingDirectory = Path;
    auto E = linux_model::validateFileOptions(*Options.LinuxFiles);
    EXPECT_TRUE(bool(E)) << Path;
    llvm::consumeError(std::move(E));
  }
  Options.LinuxFiles->WorkingDirectory = "/empty";
  auto E = linux_model::validateFileOptions(*Options.LinuxFiles);
  EXPECT_FALSE(bool(E));
  llvm::consumeError(std::move(E));
}

TEST_P(LinuxFileNamespace,
       RepeatedSeparatorsShareFileBytesAndCapturedMetadata) {
  const auto Writer = open("///files////data", 2);
  ASSERT_EQ(Writer, 3u);
  for (const char *Name :
       {"/files/data", "//files/data", "/files//data", "////files/////data"}) {
    const auto Reader = open(Name);
    ASSERT_EQ(Reader, 4u);
    EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 3), 3u);
    EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
    EXPECT_EQ(call(ServiceKind::FaccessAt, uint64_t(-100), Base, 0), 0u);
    EXPECT_EQ(call(ServiceKind::FstatAt, uint64_t(-100), Base, Base + 1024, 0),
              0u);
    EXPECT_EQ(call(ServiceKind::Fstat, Reader, Base + 1280), 0u);
    const unsigned Size = GetParam() == GuestArchitecture::X64 ? 144 : 128;
    EXPECT_EQ(bytes(Base + 1024, Size), bytes(Base + 1280, Size));
    EXPECT_EQ(call(ServiceKind::Close, Reader), 0u);
  }
  llvm::cantFail(cpu().write(Base + 512, {'x', 'y'}));
  EXPECT_EQ(call(ServiceKind::Write, Writer, Base + 512, 2), 2u);
  const auto Reader = open("/files/data");
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 3), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'x', 'y', 'c'}));
  EXPECT_EQ(call(ServiceKind::Lseek, Writer, 0, 1), 2u);
}

TEST_P(LinuxFileNamespace, SeparatorAliasesRetainDirectoryAndLinkBoundaries) {
  EXPECT_EQ(open("/files//missing"), uint64_t(-2));
  EXPECT_EQ(open("///files//data////"), uint64_t(-20));
  EXPECT_EQ(open("//files//data//child"), uint64_t(-20));
  const auto Directory = open("///empty////");
  ASSERT_EQ(Directory, 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, Directory, Base + 512, 48), 48u);
  path("//files///data");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0777),
            uint64_t(-17));
  path("//missing///child");
  EXPECT_EQ(call(ServiceKind::MkdirAt, uint64_t(-100), Base, 0777),
            uint64_t(-2));
  const auto File = open("//files/data");
  ASSERT_EQ(File, 4u);
  path("///proc//self///fd//4");
  EXPECT_EQ(
      call(ServiceKind::ReadlinkAt, uint64_t(-100), Base, Base + 512, 128),
      11u);
  EXPECT_EQ(bytes(Base + 512, 11),
            (std::vector<uint8_t>{'/', 'f', 'i', 'l', 'e', 's', '/', 'd', 'a',
                                  't', 'a'}));
  path("//proc//self//fd//4///");
  EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
}

TEST_P(LinuxFileNamespace,
       SeparatorFoldingKeepsOriginalPathLengthAndUnknownDotWalks) {
  for (const char *Name :
       {"/files//./data", "//files///../data", "files//data"}) {
    path(Name);
    EXPECT_FALSE(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0));
  }
  std::string Long(4096, '/');
  path(Long.c_str());
  EXPECT_EQ(call(ServiceKind::OpenAt, uint64_t(-100), Base, 0), uint64_t(-36));
  // The input catalogue still has one canonical spelling per object.
  Options.LinuxFiles->Files["/files//other"] = {};
  auto Error = linux_model::validateFileOptions(*Options.LinuxFiles);
  EXPECT_TRUE(bool(Error));
  llvm::consumeError(std::move(Error));
}

TEST_P(LinuxFileNamespace, IoctlTraceKeepsEntryPointerAndRefusedCallBytes) {
  auto &D = Options.LinuxFiles->Devices["/dev/example"];
  D.AccessMode = 2;
  // The output overwrites the pointer cell. Observations still read the
  // originally selected destination, not a new address from output bytes.
  D.Ioctls = {{0x12345678,
               0,
               {},
               {{0, {}, {}, std::vector<uint8_t>(8, 0)},
                {0, 0, {0xa5, 0x5a}, {0x12, 0x34}}}}};
  auto &T = Options.LinuxFiles->IoctlTrace.emplace();
  T.Requests[0x12345678] = {{0, 8, {}}, {0, 2, 0}};
  llvm::cantFail(linux_model::validateFileOptions(*Options.LinuxFiles));
  Kernel = std::make_unique<linux_model::LinuxServices>(
      cpu(), Layout, Base + 8192, Options, Report);
  const auto FD = open("/dev/example", 2);
  llvm::cantFail(cpu().writeInteger(Base + 256, Base + 512, 8));
  llvm::cantFail(cpu().write(Base + 512, {0xa5, 0x5a}));
  ProcessServiceEvent E{0x1234,
                        GetParam() == GuestArchitecture::X64 ? 16u : 29u,
                        {FD | (uint64_t(7) << 32),
                         0x12345678 | (uint64_t(9) << 32), Base + 256, 0, 0, 0},
                        {}};
  ASSERT_EQ(llvm::cantFail(Kernel->handle(E)), 0u);
  ASSERT_EQ(Report.IoctlTrace.size(), 1u);
  const auto &C = Report.IoctlTrace[0];
  EXPECT_EQ(C.PC, 0x1234u);
  EXPECT_EQ(C.FD, FD);
  EXPECT_EQ(C.Request, 0x12345678u);
  EXPECT_FALSE(C.ReturnAddress);
  ASSERT_EQ(C.Memory.size(), 2u);
  EXPECT_EQ(C.Memory[1].Address, Base + 512);
  EXPECT_EQ(C.Memory[1].Before, (std::vector<uint8_t>{0xa5, 0x5a}));
  EXPECT_EQ(C.Memory[1].After, (std::vector<uint8_t>{0x12, 0x34}));
  EXPECT_EQ(C.Memory[0].After, std::vector<uint8_t>(8, 0));
  // A repeated transaction is unsupported, not turned into a trace success.
  const auto Before = bytes(Base, 8192);
  EXPECT_FALSE(llvm::cantFail(Kernel->handle(E)));
  EXPECT_EQ(Report.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(bytes(Base, 8192), Before);
  ASSERT_EQ(Report.IoctlTrace.size(), 2u);
  EXPECT_FALSE(Report.IoctlTrace[1].Result);
  EXPECT_EQ(Report.IoctlTrace[1].Memory[0].Before,
            Report.IoctlTrace[1].Memory[0].After);
}

TEST_P(LinuxFileNamespace,
       DirentLayoutPaddingAndSeekCookiesAreIndependentOfHostABI) {
  auto FD = open("/empty");
  llvm::cantFail(cpu().write(Base + 256, std::vector<uint8_t>(64, 0xa5)));
  ASSERT_EQ(llvm::cantFail(Kernel->handle(
                {0,
                 GetParam() == GuestArchitecture::X64 ? 217u : 61u,
                 {FD, Base + 256, 48, 0, 0, 0},
                 {}})),
            48u);
  auto Actual = bytes(Base + 256, 64);
  std::vector<uint8_t> Expected(64, 0xa5);
  for (unsigned Offset : {0, 24}) {
    // Model-owned inode identities must be stable and distinct, but are not
    // captured native inode numbers. Check all other bytes independently.
    std::copy_n(Actual.begin() + Offset, 8, Expected.begin() + Offset);
    llvm::support::endian::write64le(Expected.data() + Offset + 8,
                                     Offset ? 2 : 1);
    Expected[Offset + 16] = 24;
    Expected[Offset + 17] = 0;
    Expected[Offset + 18] = 4;
    Expected[Offset + 19] = '.';
    if (Offset) {
      Expected[Offset + 20] = '.';
      Expected[Offset + 21] = 0;
    } else
      Expected[Offset + 20] = 0;
  }
  EXPECT_EQ(Actual, Expected);
  EXPECT_NE(llvm::support::endian::read64le(Actual.data()), 0u);
  EXPECT_NE(llvm::support::endian::read64le(Actual.data()),
            llvm::support::endian::read64le(Actual.data() + 24));
  EXPECT_EQ(call(ServiceKind::Getdents64, FD, UINT64_MAX, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Getdents64, FD, Base + 256, 0x80000000),
            uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Lseek, FD, 0, 1), 0u);
  ASSERT_EQ(call(ServiceKind::Getdents64, FD, Base + 256, 24), 24u);
  EXPECT_EQ(bytes(Base + 256, 24),
            std::vector<uint8_t>(Expected.begin(), Expected.begin() + 24));
}
TEST_P(LinuxFileNamespace,
       PartialWriteCommitsOnlyReadablePagesAndSharesObjectBytes) {
  const auto FD = open("/files/data", 2), Alias = *call(ServiceKind::Dup, FD),
             Reader = open("/files/data");
  llvm::cantFail(cpu().write(Base + 4092, {'W', 'X', 'Y', 'Z'}));
  llvm::cantFail(
      cpu().addressSpace()->protect(Base + 4096, 4096, UserAccessible));
  ASSERT_EQ(call(ServiceKind::Write, FD, Base + 4092, 8), 4u);
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 4u);
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 8), 4u);
  EXPECT_EQ(bytes(Base + 512, 4), (std::vector<uint8_t>{'W', 'X', 'Y', 'Z'}));
  EXPECT_EQ(call(ServiceKind::Write, FD, Base + 4096, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 4u);
  EXPECT_EQ(call(ServiceKind::Close, FD), 0u);
  EXPECT_EQ(call(ServiceKind::Write, Alias, Base + 4092, 1), 1u);
  EXPECT_FALSE(call(ServiceKind::Fstat, Reader, Base + 512));
  EXPECT_NE(Report.Diagnostic.find("invalidated"), std::string::npos);
}
TEST_P(LinuxFileNamespace, ProcCookiesSkipClosedSlotsAndEOFRequiresRewind) {
  const auto D = open("/proc/self/fd"), F = open("/files/data");
  ASSERT_EQ(D, 3u);
  ASSERT_EQ(F, 4u);
  EXPECT_EQ(call(ServiceKind::Close, 1), 0u);
  ASSERT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 1024), 144u);
  auto B = bytes(Base + 256, 144);
  const uint64_t Cookies[] = {1, 2, 4, 5, 6, 258};
  const char *Names[] = {".", "..", "0", "2", "3", "4"};
  for (unsigned I = 0; I < 6; ++I) {
    EXPECT_EQ(llvm::support::endian::read64le(B.data() + 24 * I + 8),
              Cookies[I]);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(B.data() + 24 * I + 19)),
              Names[I]);
  }
  llvm::cantFail(
      cpu().write(Base, {'/', 'p', 'r', 'o', 'c', '/', 's', 'e', 'l', 'f', 0}));
  llvm::cantFail(cpu().write(Base + 512, std::vector<uint8_t>(8, 0xa5)));
  EXPECT_EQ(call(ServiceKind::ReadlinkAt, UINT64_MAX, Base, Base + 512, 8), 4u);
  EXPECT_EQ(bytes(Base + 512, 8),
            (std::vector<uint8_t>{'1', '0', '0', '0', 0xa5, 0xa5, 0xa5, 0xa5}));
  EXPECT_EQ(call(ServiceKind::Dup, F), 1u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 1024), 0u);
  EXPECT_EQ(call(ServiceKind::Lseek, D, 3, 0), 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 24), 24u);
  EXPECT_EQ(bytes(Base + 256 + 19, 2), (std::vector<uint8_t>{'1', 0}));
}
TEST_P(LinuxFileNamespace,
       BudgetRefusalAndExclusiveOpenDoNotTruncateOrAdvance) {
  const auto F = open("/files/data", 2);
  EXPECT_FALSE(
      call(ServiceKind::OpenAt, uint64_t(-100), Base,
           0100 | (GetParam() == GuestArchitecture::X64 ? 00200000 : 040000)));

  EXPECT_EQ(open("/files/data", 0100 | 0200 | 01000 | 1), uint64_t(-17));
  EXPECT_EQ(call(ServiceKind::Lseek, F, 8191, 0), 8191u);
  EXPECT_FALSE(call(ServiceKind::Write, F, Base, 2));
  EXPECT_EQ(call(ServiceKind::Lseek, F, 0, 1), 8191u);
  EXPECT_EQ(call(ServiceKind::Lseek, F, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, F, Base + 512, 8), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
}
TEST_P(LinuxFileNamespace, AppendValidatesOriginalCursorBeforeSelectingEOF) {
  const auto F = open("/files/data", 1 | 02000),
             Alias = *call(ServiceKind::Dup, F), Reader = open("/files/data");
  ASSERT_EQ(call(ServiceKind::Lseek, F, INT64_MAX, 0), uint64_t(INT64_MAX));
  EXPECT_EQ(call(ServiceKind::Write, F, Base, 1), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), uint64_t(INT64_MAX));
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 8), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
  // Refusal leaves captured metadata valid and no bytes or cursor committed.
  EXPECT_EQ(call(ServiceKind::Fstat, Reader, Base + 512), 0u);
  EXPECT_EQ(call(ServiceKind::Write, F, UINT64_MAX, 1), uint64_t(-14));
  ASSERT_EQ(call(ServiceKind::Lseek, Alias, 100, 0), 100u);
  llvm::cantFail(cpu().write(Base + 256, {'Z'}));
  EXPECT_EQ(call(ServiceKind::Write, F, Base + 256, 1), 1u);
  EXPECT_EQ(call(ServiceKind::Lseek, Alias, 0, 1), 4u);
  EXPECT_EQ(call(ServiceKind::Read, Reader, Base + 512, 8), 1u);
  EXPECT_EQ(bytes(Base + 512, 1), (std::vector<uint8_t>{'Z'}));
}
INSTANTIATE_TEST_SUITE_P(LP64, LinuxFileNamespace,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
