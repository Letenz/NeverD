//===- LinuxBinderTests.cpp - Independent LP64 Binder memory contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Raw Linux control requests across both supported LP64 architectures.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
using linux_model::ServiceKind;
constexpr uint64_t Base = 0x20000000;
class LinuxBinder : public testing::TestWithParam<GuestArchitecture> {
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
    auto &D = Options.LinuxFiles.emplace().Devices["/dev/binder"];
    D.AccessMode = 2;
    D.Binder = true;
    llvm::cantFail(linux_model::validateFileOptions(*Options.LinuxFiles));
    Kernel = std::make_unique<linux_model::LinuxServices>(
        cpu(), Layout, Base + 8192, Options, Report);
  }
  std::optional<uint64_t> call(ServiceKind K, uint64_t A = 0, uint64_t B = 0,
                               uint64_t C = 0) {
    return llvm::cantFail(Kernel->handle(K, {0, 0, {A, B, C, 0, 0, 0}, {}}));
  }
  uint64_t open() {
    const uint8_t Path[] = "/dev/binder";
    llvm::cantFail(cpu().write(Base, Path));
    return call(ServiceKind::OpenAt, uint64_t(-100), Base, 2 | 0x800).value();
  }
  std::vector<uint8_t> read(uint64_t At, unsigned Size) {
    std::vector<uint8_t> Bytes(Size);
    llvm::cantFail(cpu().read(At, Bytes));
    return Bytes;
  }
  void record(uint64_t WriteSize, uint64_t Consumed, uint64_t WriteBuffer,
              uint64_t ReadSize, uint64_t ReadBuffer) {
    uint8_t Bytes[48];
    const uint64_t Values[] = {WriteSize, Consumed, WriteBuffer,
                               ReadSize,  0,        ReadBuffer};
    for (unsigned I = 0; I < 6; ++I)
      llvm::support::endian::write64le(Bytes + I * 8, Values[I]);
    llvm::cantFail(cpu().write(Base + 256, Bytes));
  }
};
TEST_P(LinuxBinder, GuardedVersionAndBadPointers) {
  auto FD = open();
  std::vector<uint8_t> Expected(16, 0xa5);
  llvm::cantFail(cpu().write(Base + 256, Expected));
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0046209, Base + 260), 0u);
  Expected[4] = 8;
  Expected[5] = Expected[6] = Expected[7] = 0;
  EXPECT_EQ(read(Base + 256, 16), Expected);
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0xc0046209, 1), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0x40046205, 1), uint64_t(-22));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, 1), uint64_t(-14));
  EXPECT_EQ(call(ServiceKind::Ioctl, FD, 0x40046208, 1), 0u);
}
TEST_P(LinuxBinder, CompleteRecordsResumeWritesAndPreserveReadGuards) {
  auto FD = open();
  const uint8_t Commands[] = {0xff, 0xff, 0xff, 0xff, 0x0c, 0x63, 0, 0};
  llvm::cantFail(cpu().write(Base + 512, Commands));
  llvm::cantFail(cpu().write(Base + 768, std::vector<uint8_t>(16, 0xa5)));
  record(8, 4, Base + 512, 16, Base + 768);
  auto Expected = read(Base + 256, 48);
  Expected[8] = 8;
  Expected[32] = 4;
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(read(Base + 256, 48), Expected);
  std::vector<uint8_t> Output(16, 0xa5);
  Output[0] = 0x0c;
  Output[1] = 0x72;
  Output[2] = Output[3] = 0;
  EXPECT_EQ(read(Base + 768, 16), Output);
  record(0, 0, UINT64_MAX, 16, Base + 768);
  Expected = read(Base + 256, 48);
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256),
            uint64_t(-11));
  EXPECT_EQ(read(Base + 256, 48), Expected);
  EXPECT_EQ(read(Base + 768, 16), Output);
}
TEST_P(LinuxBinder, ReadFaultStillReportsConsumedCommands) {
  auto FD = open();
  const uint8_t Command[] = {0x0c, 0x63, 0, 0};
  llvm::cantFail(cpu().write(Base + 512, Command));
  record(4, 0, Base + 512, 4, 1);
  auto Expected = read(Base + 256, 48);
  Expected[8] = 4;
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256),
            uint64_t(-14));
  EXPECT_EQ(read(Base + 256, 48), Expected);
}
TEST_P(LinuxBinder, OverlappingOutputsKeepKernelCopyOrder) {
  auto FD = open();
  record(0, 0, 1, 4, Base + 256);
  auto Expected = read(Base + 256, 48);
  Expected[32] = 4;
  ASSERT_EQ(call(ServiceKind::Ioctl, FD, 0xc0306201, Base + 256), 0u);
  EXPECT_EQ(read(Base + 256, 48), Expected);
}
TEST_P(LinuxBinder, ModelCannotMixCapturedTransactionsOrMappings) {
  for (unsigned Case = 0; Case < 3; ++Case) {
    auto Input = *Options.LinuxFiles;
    auto &D = Input.Devices.at("/dev/binder");
    if (Case == 0)
      D.Ioctls.push_back({0xc0046209, 0, {}, {}});
    if (Case == 1)
      D.ReadOnlyMappings.push_back({0, std::vector<uint8_t>(4096), {}});
    if (Case == 2)
      D.KGSL = LinuxKGSLContexts{0, 1};
    auto E = linux_model::validateFileOptions(Input);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}
INSTANTIATE_TEST_SUITE_P(Architectures, LinuxBinder,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
