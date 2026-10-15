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
    llvm::StringRef S(Name);
    std::vector<uint8_t> V(S.begin(), S.end());
    V.push_back(0);
    llvm::cantFail(cpu().write(Base, V));
    return *call(ServiceKind::OpenAt, uint64_t(-100), Base, Flags);
  }
  std::vector<uint8_t> bytes(uint64_t At, unsigned Count) {
    std::vector<uint8_t> V(Count);
    llvm::cantFail(cpu().read(At, V));
    return V;
  }
};

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
  EXPECT_EQ(call(ServiceKind::Dup, F), 1u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 1024), 0u);
  EXPECT_EQ(call(ServiceKind::Lseek, D, 3, 0), 3u);
  EXPECT_EQ(call(ServiceKind::Getdents64, D, Base + 256, 24), 24u);
  EXPECT_EQ(bytes(Base + 256 + 19, 2), (std::vector<uint8_t>{'1', 0}));
}
TEST_P(LinuxFileNamespace,
       BudgetRefusalAndExclusiveOpenDoNotTruncateOrAdvance) {
  const auto F = open("/files/data", 2);
  EXPECT_EQ(open("/files/data", 0100 | 0200 | 01000 | 1), uint64_t(-17));
  EXPECT_EQ(call(ServiceKind::Lseek, F, 8191, 0), 8191u);
  EXPECT_FALSE(call(ServiceKind::Write, F, Base, 2));
  EXPECT_EQ(call(ServiceKind::Lseek, F, 0, 1), 8191u);
  EXPECT_EQ(call(ServiceKind::Lseek, F, 0, 0), 0u);
  EXPECT_EQ(call(ServiceKind::Read, F, Base + 512, 8), 3u);
  EXPECT_EQ(bytes(Base + 512, 3), (std::vector<uint8_t>{'a', 'b', 'c'}));
}
INSTANTIATE_TEST_SUITE_P(LP64, LinuxFileNamespace,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
