//===- AndroidMutexTests.cpp - Independent Android mutex workloads --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidMutex : public testing::TestWithParam<
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
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 512});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 500000;
#endif
  }
  ProcessResult run(llvm::StringRef Entry,
                    std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry.str();
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &R, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    EXPECT_FALSE(R.ExitStatus);
  }
  void checkWaiters(unsigned Type);
  std::vector<uint8_t> object(uint16_t State, uint32_t Owner) {
    std::vector<uint8_t> Bytes(512, 0xa5);
    llvm::support::endian::write16le(Bytes.data(), State);
    llvm::support::endian::write32le(Bytes.data() + 4, Owner);
    return Bytes;
  }
};
TEST_P(AndroidMutex, AttributesHaveLongWidthAndIntegerOutputs) {
  auto R = run("mutex_attributes", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  const std::array<uint64_t, 4> Expected = {0, 0x31, 0x0123456789abcdc2,
                                            UINT64_MAX};
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8 * I),
              Expected[I]);
}
TEST_P(AndroidMutex, NormalRecursiveAndErrorCheckingKeepDistinctState) {
  for (unsigned Type = 0; Type < 3; ++Type) {
    for (unsigned Shared = 0; Shared < 2; ++Shared) {
      SCOPED_TRACE(testing::Message() << Type << ":" << Shared);
      auto R = run("mutex_sequence", {Buffer, Type, Shared});
      returned(R, 0);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      std::vector<uint8_t> Expected(512);
      for (unsigned Step = 0; Step < 5; ++Step) {
        uint16_t State = Type * 0x4000 + Shared * 0x2000;
        if (Step == 1 || Step == 2)
          State += Step == 2 && Type == 1 ? 5 : 1;
        if (Step == 4)
          State = 0xffff;
        llvm::support::endian::write16le(Expected.data() + Step * 40, State);
        if (Type && (Step == 1 || Step == 2))
          llvm::support::endian::write32le(Expected.data() + Step * 40 + 4,
                                           1000);
      }
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}
TEST_P(AndroidMutex, RecursiveDepthBoundaryPreservesErrno) {
  auto R = run("mutex_recursive_limit", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  EXPECT_EQ(llvm::support::endian::read16le(Bytes.data()), 0x5ffd);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 4), 1000u);
  EXPECT_EQ(std::vector<uint8_t>(Bytes.begin(), Bytes.begin() + 40),
            std::vector<uint8_t>(Bytes.begin() + 40, Bytes.begin() + 80));
  EXPECT_EQ(llvm::support::endian::read16le(Bytes.data() + 80), 0x5ff5);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 84), 1000u);
}
TEST_P(AndroidMutex, InitializationClearsBeforeInvalidAndAliasedAttributes) {
  auto R = run("mutex_initialization_order", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(512));
}
TEST_P(AndroidMutex, StaticMutexConstructorsAndExplicitUninitializedEntry) {
  returned(run("mutex_constructor_result"), 0);
  Options.Android->Initialize = true;
  returned(run("mutex_constructor_result"), 73);
}
TEST_P(AndroidMutex, ForeignOwnerErrorsDoNotChangeObjectOrPadding) {
  for (uint16_t State : {0x4001, 0x8001, 0x4002}) {
    auto Before = object(State, 1234);
    Options.Android->Memory[0].Bytes = Before;
    for (unsigned Operation : {1, 2, 3}) {
      auto R = run("mutex_object_call", {Buffer, Operation, 0});
      returned(R, Operation == 2 ? 1 : 16);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Before);
    }
  }
}
TEST_P(AndroidMutex, BlockingWakeAndPriorityInheritanceStopBeforeEffects) {
  for (auto [State, Owner, Operation] :
       {std::tuple<uint16_t, uint32_t, unsigned>{1, 0, 0},
        {2, 0, 2},
        {0x4001, 1234, 0},
        {0x4002, 1000, 2},
        {0xc000, 0, 0}}) {
    auto Before = object(State, Owner);
    Options.Android->Memory[0].Bytes = Before;
    auto R = run("mutex_object_call", {Buffer, Operation, 0});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Before);
  }
  auto Before = object(0x1234, 0);
  llvm::support::endian::write64le(Before.data() + 256, 0x21);
  Options.Android->Memory[0].Bytes = Before;
  auto R = run("mutex_object_call", {Buffer, 4, Buffer + 256});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Before);
}
TEST_P(AndroidMutex, DestroyedMalformedAndUnalignedObjectsFailExplicitly) {
  for (uint16_t State : {0xffff, 3, 0x4004, 0x8005}) {
    Options.Android->Memory[0].Bytes = object(State, 0);
    auto R = run("mutex_object_call", {Buffer, 0, 0});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
  EXPECT_EQ(run("mutex_object_call", {Buffer + 2, 0, 0}).Stop,
            ProcessStopReason::RuntimeFailure);
  EXPECT_EQ(run("mutex_attribute_call", {Buffer + 4, 0, 0}).Stop,
            ProcessStopReason::RuntimeFailure);
  returned(run("mutex_attribute_call", {1, 2, UINT64_MAX}), 22);
}
TEST_P(AndroidMutex, NormalLockDoesNotWriteUnusedOwnerOrReservedBytes) {
  auto &Before = Options.Android->Memory[0].Bytes;
  Before.assign(8192, 0xa5);
  llvm::support::endian::write16le(Before.data() + 4092, 0);
  Options.Android->ReadMemory = {{Buffer + 4092, 40}};
  auto R = run("mutex_readonly_tail", {Buffer + 4092});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  std::vector<uint8_t> Expected(40, 0xa5);
  llvm::support::endian::write16le(Expected.data(), 1);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}
TEST_P(AndroidMutex, DynamicNamesKeepProviderIdentityAndClosedCallsFail) {
  Options.Android->Libraries["libpthread-model.so"] = {"pthread_mutex_lock",
                                                       "pthread_mutex_unlock"};
  auto R = run("mutex_dynamic", {Buffer, 0});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  unsigned Calls = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Library == "libpthread-model.so" &&
        llvm::StringRef(Call.Name).starts_with("pthread_mutex_")) {
      ++Calls;
      const size_t Offset = Call.Name == "pthread_mutex_lock" ? 0 : 8;
      EXPECT_EQ(Call.PC, llvm::support::endian::read64le(
                             R.MemorySnapshots[0].Bytes.data() + Offset));
      EXPECT_EQ(Call.Result, 0u);
    }
  }
  EXPECT_EQ(Calls, 2u);
  R = run("mutex_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "pthread_mutex_lock");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
void AndroidMutex::checkWaiters(unsigned Type) {
  Options.Android->ThreadLimit = 4;
  Options.Android->ReadMemory = {{Buffer, 1024}};
  Options.Limits.Instructions = 300000;
  for (unsigned Shared = 0; Shared != 2; ++Shared) {
    SCOPED_TRACE(testing::Message() << "type=" << Type << " shared=" << Shared);
    // The large quantum exercises an unlocker acquiring again before its
    // woken competitor runs. Short quanta exercise suspension in callers.
    for (uint64_t Quantum : {7u, 37u, 4093u}) {
      SCOPED_TRACE(Quantum);
      Options.InstructionQuantum = Quantum;
      auto R = run("mutex_waiters", {Buffer, Type, Shared, 1});
      returned(R, 0);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      const auto &Bytes = R.MemorySnapshots[0].Bytes;
      auto Word = [&](unsigned I) {
        return llvm::support::endian::read64le(Bytes.data() + 256 + I * 8);
      };
      const uint16_t Base = Type * 0x4000 + Shared * 0x2000;
      EXPECT_EQ(Word(0), Base + (Type == 1 ? 6u : 2u));
      if (Type == 1)
        EXPECT_EQ(Word(1), Base + 2u);
      EXPECT_EQ(Word(3), Base);
      EXPECT_EQ(Word(4), 77u);
      EXPECT_EQ(Word(32), 3u);
      if (Quantum == 4093) {
        EXPECT_EQ(Word(5), Base + 1u);
        EXPECT_EQ(Word(2), Base + 2u);
      }
      for (unsigned I = 0; I < 3; ++I) {
        EXPECT_EQ(Word(8 + I), Base + 2u) << I;
        EXPECT_EQ(Word(16 + I), 1001u + I) << I;
        EXPECT_EQ(Word(24 + I), 71u + I) << I;
      }
      unsigned Locks = 0, Tries = 0;
      for (const auto &Call : R.NativeCalls) {
        if (Call.Name == "pthread_mutex_lock") {
          ++Locks;
          EXPECT_EQ(Call.Result, 0u);
        } else if (Call.Name == "pthread_mutex_trylock") {
          ++Tries;
          EXPECT_EQ(Call.Result, 16u);
        }
      }
      EXPECT_EQ(Locks, Type == 1 ? 6u : 5u);
      EXPECT_EQ(Tries, 3u);
      ASSERT_EQ(R.NativeThreads.size(), 4u);
      for (const auto &T : R.NativeThreads) {
        EXPECT_TRUE(T.Finished);
        EXPECT_FALSE(T.Waiting);
      }
    }
  }
}

TEST_P(AndroidMutex, NormalWaitersRetryAfterWakeWithoutDuplicatingCalls) {
  checkWaiters(0);
}
TEST_P(AndroidMutex, RecursiveWaitersWakeOnlyAfterFinalRelease) {
  checkWaiters(1);
}
TEST_P(AndroidMutex, ErrorCheckingWaitersAcquireWithTheirOwnIdentity) {
  checkWaiters(2);
}

TEST_P(AndroidMutex, WokenLocksRevalidateStateAndPermissionsBeforeCompletion) {
  Options.Android->ThreadLimit = 2;
  Options.Android->ReadMemory = {{Buffer + 4096, 16}};
  Options.InstructionQuantum = 4093;
  for (unsigned Mode : {0u, 1u, 2u, 3u, 6u}) {
    SCOPED_TRACE(Mode);
    auto R = run("mutex_wait_failure", {Buffer, Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    unsigned Pending = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_mutex_lock" && Call.ThreadID == 1001u) {
        ++Pending;
        EXPECT_FALSE(Call.Result);
      }
    EXPECT_EQ(Pending, 1u);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[1].Waiting);
    EXPECT_FALSE(R.NativeThreads[1].Finished);
  }
}

TEST_P(AndroidMutex, DeadlockAndBudgetStopsLeaveWaitersIncomplete) {
  Options.Android->ThreadLimit = 2;
  Options.InstructionQuantum = 7;
  Options.Limits.Instructions = 5000;
  for (unsigned Mode : {4u, 5u}) {
    auto R = run("mutex_wait_failure", {Buffer, Mode});
    EXPECT_EQ(R.Stop, Mode == 4 ? ProcessStopReason::UnsupportedService
                                : ProcessStopReason::InstructionLimit)
        << R.Diagnostic;
    if (Mode == 5)
      EXPECT_EQ(R.Instructions, Options.Limits.Instructions);
    ASSERT_EQ(R.NativeThreads.size(), 2u);
    EXPECT_TRUE(R.NativeThreads[1].Waiting);
    unsigned Pending = 0;
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == "pthread_mutex_lock" && Call.ThreadID == 1001u) {
        ++Pending;
        EXPECT_FALSE(Call.Result);
      }
    EXPECT_EQ(Pending, 1u);
  }
}

TEST_P(AndroidMutex, NonzeroFutexPaddingCannotInventASleepingWaiter) {
  Options.Android->ThreadLimit = 2;
  const auto Before = object(1, 0);
  Options.Android->Memory[0].Bytes = Before;
  auto R = run("mutex_object_call", {Buffer, 0, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  ASSERT_EQ(R.NativeCalls.size(), 1u);
  EXPECT_FALSE(R.NativeCalls[0].Result);
  ASSERT_EQ(R.NativeThreads.size(), 1u);
  EXPECT_FALSE(R.NativeThreads[0].Waiting);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Before);
}

INSTANTIATE_TEST_SUITE_P(
    OptimizationPackingAndBackend, AndroidMutex,
    testing::Combine(testing::Values("mutex-O0-none", "mutex-O0-android",
                                     "mutex-O0-relr", "mutex-O2-none",
                                     "mutex-O2-android", "mutex-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF)),
    [](const testing::TestParamInfo<AndroidMutex::ParamType> &Info) {
      std::string Name = std::get<0>(Info.param);
      std::replace(Name.begin(), Name.end(), '-', '_');
      return Name + "_" + executionBackendName(std::get<1>(Info.param));
    });

TEST(AndroidMutexMemory, PermissionFailureCannotPublishHalfAnOwnedState) {
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::Unicorn;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(Configuration, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  auto &Space = *CPU.addressSpace();
  ASSERT_EQ(
      llvm::toString(Space.map(Buffer, 8192, Read | Write | UserAccessible)),
      "");
  std::array<uint8_t, 40> Before{};
  llvm::support::endian::write16le(Before.data(), 0x4000);
  ASSERT_EQ(llvm::toString(CPU.write(Buffer + 4092, Before)), "");
  ASSERT_EQ(
      llvm::toString(Space.protect(Buffer + 4096, 4096, Read | UserAccessible)),
      "");
  ProcessOptions Options;
  Options.Android.emplace();
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  linux_model::LinuxMemory Memory(Space, Layout, Buffer + 8192, Options);
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  auto Budget = ExecutionBudget::create(Options.Limits);
  ASSERT_TRUE(bool(Budget)) << llvm::toString(Budget.takeError());
  const android_model::LinkedImage Linked{};
  android_model::Bionic Model(CPU, Memory, Layout, Options, Result, **Budget,
                              Linked);
  for (const char *Name : {"pthread_mutex_lock", "pthread_mutex_init"}) {
    NativeCallEvent Call{};
    Call.Name = Name;
    Call.Arguments[0] = Buffer + 4092;
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("invalid guest pointer"),
              std::string::npos);
    std::array<uint8_t, 40> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer + 4092, After)), "");
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
