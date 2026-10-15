//===- AndroidDeviceTests.cpp - Shared explicit device transactions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Executes independent callers and checks refused transactions before effects.
//===----------------------------------------------------------------------===//
#include "LinuxDeviceTestData.h"
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidDevices : public testing::TestWithParam<
                           std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  static constexpr uint64_t Buffer = 0x21000000;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->Libraries["libdevice.so"] = {"ioctl", "mmap", "dup",
                                                  "dup3"};
    Options.LinuxFiles.emplace().Devices["/fixture/control"] =
        deviceTestInput();
    Options.Limits.Instructions = 200000;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
};

TEST_P(AndroidDevices, DuplicatesShareCursorsTransactionsAndOutputTargets) {
  Options.LinuxFiles->Files["/fixture/bytes"] = {'a', 'b', 'c'};
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_duplicates", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_EQ(R.StandardOutput, "out");
    EXPECT_EQ(R.StandardError, "err");
  }
}

TEST_P(AndroidDevices, TimedDeviceWritesSurviveAliasesRetirementAndRemapping) {
  Options.LinuxFiles->Devices.at("/fixture/control") =
      updatingDeviceTestInput();
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  auto R = run("devices_updates");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}

TEST_P(AndroidDevices, ExplicitInstructionTimeAdvancesAPureGuestPollingLoop) {
  auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
  D.ReadOnlyMappings[0].Updates = {{1000000, 0, {0x9a}}};
  Options.LinuxTime.emplace().NanosecondsPerInstruction = 100;
  Options.InstructionQuantum = 1024;
  auto R = run("devices_busy_poll");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  EXPECT_GE(R.Instructions, 10000u);
  EXPECT_LT(R.Instructions, 13000u);
  Options.LinuxTime->NanosecondsPerInstruction = 0;
  Options.Limits.Instructions = 20000;
  R = run("devices_busy_poll");
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
}

TEST_P(AndroidDevices, RunnableChildAndSleepingParentShareProducerTime) {
  Options.LinuxFiles->Devices.at("/fixture/control")
      .ReadOnlyMappings[0]
      .Updates = {{1000000, 0, {0x9a}}};
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  Options.LinuxTime->NanosecondsPerInstruction = 100;
  Options.InstructionQuantum = 1024;
  Options.Android->ThreadLimit = 2;
  Options.Android->DrainThreads = true;
  auto R = run("devices_thread_updates");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  EXPECT_GE(R.Instructions, 10000u);
}

TEST_P(AndroidDevices, ContextOwnershipAndTimestampQueriesShareMappedBytes) {
  Options.LinuxFiles->Devices.at("/fixture/control") = contextDeviceTestInput();
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_contexts", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidDevices, GPUCommandsWritePinnedRAMAndRetireOwnedTimestamps) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_gpu_commands", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidDevices, RejectedGPUProgramsCannotPublishEarlierWrites) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096},
                                   {Buffer + 4096 + 20, 4},
                                   {0x22000000 + 40, 4},
                                   {0x22000000 + 48, 4}};
    auto R = run("devices_gpu_rejected", {Mode, Buffer});
    EXPECT_EQ(R.Stop, Mode == 1 ? ProcessStopReason::RuntimeFailure
                                : ProcessStopReason::UnsupportedService)
        << R.Diagnostic;
    ASSERT_EQ(R.MemorySnapshots.size(), 4u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
    EXPECT_EQ(R.MemorySnapshots[1].Bytes,
              (std::vector<uint8_t>{0xef, 0xbe, 0xad, 0xde}));
    EXPECT_EQ(R.MemorySnapshots[2].Bytes, std::vector<uint8_t>(4, 0));
    EXPECT_EQ(R.MemorySnapshots[3].Bytes, std::vector<uint8_t>(4, 0));
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidDevices, ContextRefusalsDoNotInventOutputs) {
  Options.LinuxFiles->Devices.at("/fixture/control") = contextDeviceTestInput();
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  for (uint64_t Mode = 0; Mode != 7; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096}};
    auto R = run("devices_context_unsupported", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    auto Expected = std::vector<uint8_t>(4096, 0xa5);
    llvm::support::endian::write32le(Expected.data(), Mode >= 3   ? 0x8052
                                                      : Mode == 0 ? 0x20012
                                                      : Mode == 1 ? 0x12
                                                                  : 1);
    llvm::support::endian::write32le(Expected.data() + 4, Mode >= 3 ? 1
                                                          : Mode == 2
                                                              ? 3
                                                              : 0xffffffffU);
    if (Mode == 2)
      llvm::support::endian::write32le(Expected.data() + 8, 0xdeadbeefU);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidDevices, RawNamedVariadicAndDynamicCallsShareTransactions) {
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_sequence", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}

TEST_P(AndroidDevices, DeviceMappingsOutliveDescriptorsAcrossEveryCallRoute) {
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_mapping", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}

TEST_P(AndroidDevices, DeviceMappingAccessFaultsReachTheGuest) {
  for (uint64_t Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("devices_mapping_fault", {Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
    ASSERT_TRUE(R.LastCPUExit && R.LastCPUExit->Fault);
    const auto &Fault = *R.LastCPUExit->Fault;
    EXPECT_EQ(Fault.Kind, Mode == 2 ? BackendFaultKind::UnmappedMemory
                                    : BackendFaultKind::Protection);
    EXPECT_EQ(Fault.Access,
              Mode == 1 ? BackendAccessKind::Write : BackendAccessKind::Read);
    auto Map = std::find_if(R.Services.begin(), R.Services.end(),
                            [](const auto &S) { return S.Number == 222; });
    ASSERT_NE(Map, R.Services.end());
    ASSERT_TRUE(Map->Result);
    EXPECT_LT(*Map->Result, uint64_t(-4095));
    EXPECT_EQ(Fault.Address, Map->Result);
  }
}

TEST_P(AndroidDevices, ExplicitOpenErrorsAndInactiveProvidersRetainBoundaries) {
  Options.LinuxFiles->Devices.at("/fixture/control").OpenErrno = 13;
  auto R = run("devices_open_error");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  Options.LinuxFiles->Devices.at("/fixture/control").OpenErrno = 0;
  R = run("devices_sequence", {4});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  Options.LinuxFiles->Devices.at("/fixture/control").AccessMode = 1;
  R = run("devices_sequence", {0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("open access mode"), std::string::npos);
}

TEST_P(AndroidDevices, CompletePreflightPreservesEveryByteOnRefusal) {
  for (unsigned Mode = 0; Mode != 7; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096}};
    auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
    D.Ioctls = {{0xa1000001, 0, {}, {{0, {}, {0xa5}, {0x42}}}}};
    if (Mode == 0)
      D.Ioctls.clear();
    if (Mode == 1)
      D.Ioctls[0].Request ^= 1;
    if (Mode == 2)
      D.Ioctls[0].Memory[0].Expected = {0xa4};
    if (Mode == 3)
      D.Ioctls[0].Memory.push_back({4096, {}, {}, {0x99}});
    if (Mode == 4)
      D.Ioctls[0].Memory.push_back({0, 8, {}, {0x99}});
    if (Mode == 5)
      D.Ioctls[0].Memory.push_back({0, {}, {0x42}, {0x99}});
    auto R = run(Mode == 6 ? "devices_pending_readonly" : "devices_pending",
                 {0, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
  }
}

TEST_P(AndroidDevices, PointersAndAliasedInputsAreResolvedBeforeOutputs) {
  std::vector<uint8_t> Bytes(4096, 0xa5);
  llvm::support::endian::write64le(Bytes.data() + 8, Buffer + 64);
  Options.Android->Memory = {{Buffer, 4096, Bytes, false}};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
  D.Ioctls = {{0xa1000001,
               37,
               {},
               {{8, {}, {}, std::vector<uint8_t>(8, 0)},
                {0, 8, {0xa5}, {0x19}},
                {64, {}, {0xa5}, {0x27}}}}};
  auto R = run("devices_pending", {1, Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 37u);
  std::fill(Bytes.begin() + 8, Bytes.begin() + 16, 0);
  Bytes[64] = 0x27;
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
}

TEST(AndroidDeviceMemory, RefusedTransactionsDoNotAdvanceTheOpenDescription) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::Unicorn;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(C, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  constexpr uint64_t Buffer = 0x21000000;
  ASSERT_EQ(llvm::toString(CPU.addressSpace()->map(
                Buffer, 4096, Read | Write | UserAccessible)),
            "");
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, {'/', 'd', 0})), "");
  ProcessOptions Options;
  auto &Files = Options.LinuxFiles.emplace();
  Files.DescriptorLimit = 4;
  Files.Devices["/d"] = {
      2, 0, {{0x87654321, -13, Buffer + 64, {{0, {}, {0xa5}, {0x42}}}}}};
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                    Result);
  auto Open = [&] {
    return llvm::cantFail(
        Kernel.handle(linux_model::ServiceKind::OpenAt,
                      {0, 56, {uint64_t(-100), Buffer, 2, 0, 0, 0}, {}}));
  };
  auto Ioctl = [&](uint64_t Request, uint64_t Argument) {
    return llvm::cantFail(
        Kernel.handle(linux_model::ServiceKind::Ioctl,
                      {0, 29, {3, Request, Argument, 0, 0, 0}, {}}));
  };
  EXPECT_EQ(Open(), 3u);
  EXPECT_EQ(Open(), uint64_t(-24));
  EXPECT_EQ(llvm::cantFail(Kernel.handle(linux_model::ServiceKind::Dup,
                                         {0, 23, {3, 0, 0, 0, 0, 0}, {}})),
            uint64_t(-24));
  EXPECT_FALSE(Ioctl(0x87654320, Buffer + 64));
  EXPECT_FALSE(Ioctl(0x87654321, Buffer + 65));
  EXPECT_FALSE(Ioctl(0x87654321, Buffer + 64));
  ASSERT_EQ(llvm::toString(CPU.write(Buffer + 64, {0xa5})), "");
  EXPECT_EQ(Ioctl(0x1234567887654321ULL, Buffer + 64), uint64_t(-13));
  EXPECT_EQ(llvm::cantFail(CPU.readInteger(Buffer + 64, 1)), 0x42u);
  EXPECT_FALSE(Ioctl(0x87654321, Buffer + 64));
  EXPECT_EQ(llvm::cantFail(CPU.readInteger(Buffer + 64, 1)), 0x42u);
  for (auto Kind :
       {linux_model::ServiceKind::Read, linux_model::ServiceKind::Lseek,
        linux_model::ServiceKind::Write}) {
    EXPECT_FALSE(llvm::cantFail(
        Kernel.handle(Kind, {0, 0, {3, Buffer + 64, 1, 0, 0, 0}, {}})));
    EXPECT_EQ(Result.Diagnostic, linux_model::DeviceOperation);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidDevices,
    testing::Combine(testing::Values("devices-O0-none", "devices-O0-android",
                                     "devices-O0-relr", "devices-O2-none",
                                     "devices-O2-android", "devices-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
