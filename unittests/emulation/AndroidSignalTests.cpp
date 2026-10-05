//===- AndroidSignalTests.cpp - Shared dispositions and distinct ABIs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr LinuxSignalAction Initial{0x123456789abcdef0, 0x10000004,
                                    0xfedcba9876543210, 0x8000000000000001};
class AndroidSignals : public testing::TestWithParam<
                           std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
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
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.LinuxSignals.emplace();
    Options.LinuxSignals->Actions = {{11, Initial}, {64, Initial}, {9, {}}};
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  ProcessResult call(uint64_t Op, uint64_t Signal = 11, uint64_t New = 0,
                     uint64_t Old = 0, uint64_t Size = 8, uint64_t RO = 0) {
    return run("signal_call", {Op, Signal, New, Old, Size, Buffer + 512, RO});
  }
  static void put(std::vector<uint8_t> &Bytes, unsigned At, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + At, Value);
  }
  static void bionic(std::vector<uint8_t> &Bytes, unsigned At,
                     const LinuxSignalAction &Action) {
    llvm::support::endian::write32le(Bytes.data() + At, Action.Flags);
    put(Bytes, At + 8, Action.Handler);
    put(Bytes, At + 16, Action.Mask);
    put(Bytes, At + 24, Action.Restorer);
  }
  static void kernel(std::vector<uint8_t> &Bytes, unsigned At,
                     const LinuxSignalAction &Action) {
    put(Bytes, At, Action.Handler);
    put(Bytes, At + 8, Action.Flags);
    put(Bytes, At + 16, Action.Restorer);
    put(Bytes, At + 24, Action.Mask);
  }
  static void returned(const ProcessResult &R, uint64_t Value,
                       uint32_t Error = 73) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 512),
              Error);
  }
  std::vector<uint8_t> &input() { return Options.Android->Memory[0].Bytes; }
};

TEST_P(AndroidSignals, QueryTranslatesBothLayoutsPreservesPaddingAndErrno) {
  for (unsigned Op : {0, 1, 2, 3}) {
    SCOPED_TRACE(Op);
    auto R = call(Op, 0x123456780000000b, 0, Buffer + 8);
    returned(R, 0);
    auto Expected = std::vector<uint8_t>(8192, 0xa5);
    if (Op < 2)
      bionic(Expected, 8, Initial);
    else
      kernel(Expected, 8, Initial);
    llvm::support::endian::write32le(Expected.data() + 512, 73);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  returned(call(0, 64, 0, Buffer), 0);
  returned(call(2, 9, 0, Buffer), 0);
}

TEST_P(AndroidSignals, MissingObservationsNeverBecomeDefaultHandlers) {
  for (bool Enabled : {false, true}) {
    if (Enabled)
      Options.LinuxSignals.emplace();
    else
      Options.LinuxSignals.reset();
    for (unsigned Op : {0, 1, 2, 3}) {
      auto R = call(Op, 11, 0, Buffer);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
      returned(call(Op), 0);
    }
  }
}

TEST_P(AndroidSignals, InvalidArgumentsRespectKernelAndBionicBoundaries) {
  for (uint64_t Signal : {uint64_t(0), uint64_t(65), UINT64_MAX}) {
    returned(call(0, Signal), UINT64_MAX, 22);
    returned(call(2, Signal, 0, 1), uint64_t(0) - 22);
    returned(call(2, Signal, 1), uint64_t(0) - 14);
  }
  returned(call(2, 0, 1, 1, 7), uint64_t(0) - 22);
  returned(call(3, 11, 1), UINT64_MAX, 14);
  for (unsigned Op : {0, 1}) {
    auto R = call(Op, 0, 0, Buffer);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("output after a failed"), std::string::npos);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
    EXPECT_EQ(call(Op, 11, 1).Stop, ProcessStopReason::RuntimeFailure);
  }
}

TEST_P(AndroidSignals, ReplacementsShareStateAndRetainFailedCopySideEffects) {
  auto R = run("signal_sequence", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  auto Expected = std::vector<uint8_t>(8192, 0xa5);
  const uint64_t Values[] = {0, 0, uint64_t(0) - 14, 0, 73};
  for (size_t I = 0; I < std::size(Values); ++I)
    put(Expected, I * 8, Values[I]);
  bionic(Expected, 64, Initial);
  const LinuxSignalAction New{0x123456789abc0018, 0x94000004,
                              0xfedcba9876540018, UINT64_MAX};
  bionic(Expected, 128, New);
  kernel(Expected, 192,
         {New.Handler, 0xffffffff94000004, New.Restorer, 0xfffffff87ffbfeff});
  const LinuxSignalAction Last{1, 0x10000001, 0x1122334455667788, UINT64_MAX};
  kernel(Expected, 256, Last);
  bionic(Expected, 320,
         {Last.Handler, Last.Flags, Last.Restorer, 0xfffffffffffbfeff});
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidSignals, OverlappingObjectsCaptureInputBeforeOutput) {
  for (unsigned Op : {0, 2}) {
    const LinuxSignalAction New{1, 0x10000000, 0x1122334455667788, UINT64_MAX};
    input().assign(8192, 0xa5);
    if (Op == 0)
      bionic(input(), 0, New);
    else
      kernel(input(), 0, New);
    auto Expected = input();
    if (Op == 0)
      bionic(Expected, 8, Initial);
    else
      kernel(Expected, 8, Initial);
    llvm::support::endian::write32le(Expected.data() + 512, 73);
    auto R = call(Op, 11, Buffer, Buffer + 8);
    returned(R, 0);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidSignals, KernelOnlyActionsAndUnsupportedFlagsDoNotSucceed) {
  input().assign(8192, 0);
  returned(call(0, 9, Buffer), UINT64_MAX, 22);
  returned(call(2, 19, Buffer), uint64_t(0) - 22);
  for (unsigned Op : {0, 2}) {
    if (Op == 0)
      bionic(input(), 0, {1, 0x400, 0, 0});
    else
      kernel(input(), 0, {1, 0x400, 0, 0});
    auto R = call(Op, 11, Buffer);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("action flags"), std::string::npos);
  }
}

TEST_P(AndroidSignals, OutputFaultsKeepUserStoresDistinctFromKernelCopies) {
  for (unsigned Op : {0, 1, 2, 3}) {
    auto R = call(Op, 11, 0, Buffer + 4096, 8, Buffer + 4096);
    if (Op < 2)
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    else
      returned(R, Op == 2 ? uint64_t(0) - 14 : UINT64_MAX, Op == 2 ? 73 : 14);
  }
  auto R = call(2, 11, 0, Buffer + 4080, 8, Buffer + 4096);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("partial Linux signal"), std::string::npos);
}

TEST_P(AndroidSignals, DynamicCallsKeepTheirProviderAndClosedHandleBoundary) {
  Options.Android->Libraries["libsignals-model.so"] = {"sigaction"};
  auto R = run("signal_dynamic", {Buffer, 0});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "sigaction");
  EXPECT_EQ(R.NativeCalls.back().Library, "libsignals-model.so");
  EXPECT_EQ(run("signal_dynamic", {Buffer, 1}).Stop,
            ProcessStopReason::UnsupportedService);
}

TEST(AndroidSignalMemory,
     EarlierBionicFieldsAndInstalledActionSurviveLaterFault) {
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
  ASSERT_EQ(llvm::toString(CPU.addressSpace()->map(
                Buffer, 4096, Read | Write | UserAccessible)),
            "");
  auto Bytes = std::vector<uint8_t>(4096, 0xa5);
  llvm::support::endian::write32le(Bytes.data(), 4);
  llvm::support::endian::write64le(Bytes.data() + 8, 1);
  llvm::support::endian::write64le(Bytes.data() + 16, 0);
  llvm::support::endian::write64le(Bytes.data() + 24, 0);
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, Bytes)), "");
  ProcessOptions Options;
  Options.Android.emplace();
  Options.LinuxSignals.emplace();
  Options.LinuxSignals->Actions[11] = Initial;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                    Result);
  const android_model::LinkedImage Linked{};
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                              Linked);
  NativeCallEvent Call{};
  Call.Name = "sigaction";
  Call.Arguments = {11, Buffer, Buffer + 4092};
  auto R = Model.invoke(Call);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  llvm::support::endian::write32le(Bytes.data() + 4092, Initial.Flags);
  auto After = std::vector<uint8_t>(4096);
  ASSERT_EQ(llvm::toString(CPU.read(Buffer, After)), "");
  EXPECT_EQ(After, Bytes);
  auto Current = Kernel.signalAction(11, nullptr, true);
  ASSERT_TRUE(Current);
  EXPECT_EQ(Current->Status, 0u);
  EXPECT_EQ(Current->Previous.Handler, 1u);
  EXPECT_EQ(Current->Previous.Flags, 4u);
  EXPECT_EQ(Current->Previous.Mask, 0u);
  EXPECT_EQ(Current->Previous.Restorer, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidSignals,
    testing::Combine(testing::Values("signals-O0-none", "signals-O0-android",
                                     "signals-O0-relr", "signals-O2-none",
                                     "signals-O2-android", "signals-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
