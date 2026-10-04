//===- AndroidFinalizerTests.cpp - Native C++ destruction ABI checks ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidFinalizers : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 256});
    Options.Android->TraceLimit = 4096;
    Options.InstructionQuantum = 3;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry;
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
  void returned(const ProcessResult &R, uint64_t Value = 73) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    if (Options.Android->TraceLimit) {
      EXPECT_EQ(R.Trace.size(), R.Instructions);
      EXPECT_FALSE(R.TraceTruncated);
    }
    for (const auto &Call : R.NativeCalls)
      EXPECT_TRUE(Call.Result.has_value()) << Call.Name;
  }
  void memory(const ProcessResult &R, llvm::ArrayRef<uint64_t> Header,
              llvm::ArrayRef<uint64_t> Calls) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(256);
    for (size_t I = 0; I < Header.size(); ++I)
      llvm::support::endian::write64le(Expected.data() + I * 8, Header[I]);
    for (size_t I = 0; I < Calls.size(); ++I)
      llvm::support::endian::write64le(Expected.data() + (8 + I) * 8, Calls[I]);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  void incompleteFinalization(const ProcessResult &R) {
    EXPECT_FALSE(R.ReturnValue);
    bool Found = false;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "__cxa_finalize") {
        Found = true;
        EXPECT_FALSE(Call.Result);
      }
    }
    EXPECT_TRUE(Found);
  }
};

TEST_P(AndroidFinalizers, ConstructorRegistrationDSOOrderDuplicatesAndReuse) {
  for (bool Initialize : {true, false}) {
    Options.Android->Initialize = Initialize;
    auto R = run("cxa_sequence", {Buffer});
    returned(R);
    memory(R, {6, 3, 3, 5, Initialize ? 1u : 0u, 1, 1000, 73},
           {33, 33, 11, 44, 22, 55});
  }
}
TEST_P(AndroidFinalizers,
       RecursiveFinalizeAndOnceCallbacksPreserveContinuations) {
  auto R = run("cxa_nested", {Buffer});
  returned(R);
  memory(R, {7, 1, 2, 0, 0, 0, 1000}, {33, 66, 77, 11, 99, 88, 22});
  std::vector<std::string> Names;
  for (const auto &Call : R.NativeCalls)
    Names.push_back(Call.Name);
  EXPECT_EQ(
      Names,
      (std::vector<std::string>{
          "__cxa_atexit", "__cxa_atexit", "__cxa_atexit", "__cxa_atexit",
          "__cxa_finalize", "getuid", "pthread_once", "getuid", "__cxa_atexit",
          "__cxa_finalize", "getuid", "getuid", "__cxa_atexit",
          "__cxa_finalize", "getuid", "__cxa_finalize", "getuid", "getuid"}));
}
TEST_P(AndroidFinalizers, NewlyRegisteredCallbacksPrecedeOlderCallbacks) {
  auto R = run("cxa_append", {Buffer});
  returned(R);
  memory(R, {4, 3, 0, 0, 0, 0, 1000}, {22, 33, 11, 44});
}
TEST_P(AndroidFinalizers, FunctionReturnDoesNotFinalizeOrLeakAcrossWorkloads) {
  auto R = run("cxa_register_only");
  returned(R);
  memory(R, {}, {});
  ASSERT_EQ(R.NativeCalls.size(), 2u);
  EXPECT_EQ(R.NativeCalls.back().Name, "__cxa_atexit");
  R = run("cxa_sequence", {Buffer});
  returned(R);
  memory(R, {6, 3, 3, 5, 1, 1, 1000, 73}, {33, 33, 11, 44, 22, 55});
}
TEST_P(AndroidFinalizers,
       RegistrationStoresOpaquePointersUntilActualInvocation) {
  Options.Android->Initialize = false;
  for (uint64_t Entry : {uint64_t(0), uint64_t(1), Buffer, UINT64_MAX}) {
    returned(run("cxa_supplied", {Entry, UINT64_MAX, UINT64_MAX, 0}), 0);
    auto R = run("cxa_supplied", {Entry, UINT64_MAX, UINT64_MAX, 1});
    if (!Entry) {
      returned(R, 0);
      continue;
    }
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    incompleteFinalization(R);
    EXPECT_TRUE(R.MemorySnapshots.empty());
    ASSERT_EQ(R.NativeCalls.size(), 2u);
    EXPECT_EQ(R.NativeCalls.front().Result, 0u);
  }
}
TEST_P(AndroidFinalizers, CallbackArgumentsAndDSOTokensKeepEveryBit) {
  Options.Android->Initialize = false;
  for (uint64_t Argument : {uint64_t(0), uint64_t(0x100000009), UINT64_MAX}) {
    for (uint64_t DSO : {uint64_t(0), UINT64_MAX}) {
      auto R = run("cxa_arguments", {Buffer, Argument, DSO});
      returned(R);
      memory(R, {1, 0, 0, 0, 0, 0, 1000}, {Argument});
    }
  }
}
TEST_P(AndroidFinalizers, UnsupportedCallbackCannotCompleteItsParentImport) {
  auto R = run("cxa_stop", {Buffer, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  incompleteFinalization(R);
  memory(R, {1, 0, 0, 0, 0, 0, 1000}, {33});
  EXPECT_EQ(R.NativeCalls.back().Name, "unknown_finalizer");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidFinalizers, CallbackSharesInstructionAndEventBudgets) {
  Options.Android->Initialize = false;
  Options.Limits.Instructions = 500;
  auto R = run("cxa_stop", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 500u);
  incompleteFinalization(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data()),
            1u);
  Options.Limits.Instructions = 10000;
  Options.Limits.Events = 4;
  R = run("cxa_stop", {Buffer, 0});
  EXPECT_EQ(R.Stop, ProcessStopReason::EventLimit) << R.Diagnostic;
  EXPECT_EQ(R.Events, 4u);
  incompleteFinalization(R);
  memory(R, {1, 0, 0, 0, 0, 0, 1000}, {33});
}
TEST_P(AndroidFinalizers, StackCorruptionAndRawExitDoNotReturnFromFinalize) {
  auto R = run("cxa_stop", {Buffer, 2});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  incompleteFinalization(R);
  EXPECT_TRUE(R.MemorySnapshots.empty());
  EXPECT_NE(R.Diagnostic.find("did not restore its stack"), std::string::npos);
  R = run("cxa_stop", {Buffer, 3});
  EXPECT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
  EXPECT_EQ(R.ExitStatus, 19);
  incompleteFinalization(R);
  memory(R, {1, 0, 0, 0, 0, 0, 1000}, {33});
}
TEST_P(AndroidFinalizers, DynamicCallsKeepLookupIdentityAndProviderLifetime) {
  Options.Android->Libraries["libfinalize-model.so"] = {"__cxa_atexit",
                                                        "__cxa_finalize"};
  for (bool Closed : {false, true}) {
    auto R = run("cxa_dynamic", {Buffer, Closed});
    if (Closed) {
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      incompleteFinalization(R);
      memory(R, {}, {});
    } else {
      returned(R);
      memory(R, {1, 0, 0, 0, 0, 0, 1000}, {0x100000009});
    }
    for (const char *Name : {"__cxa_atexit", "__cxa_finalize"}) {
      const NativeCallEvent *Lookup = nullptr, *Call = nullptr;
      for (const auto &Event : R.NativeCalls) {
        if (Event.Name == "dlsym" && Event.Symbol == Name)
          Lookup = &Event;
        if (Event.Name == Name)
          Call = &Event;
      }
      ASSERT_NE(Lookup, nullptr);
      ASSERT_NE(Call, nullptr);
      EXPECT_EQ(Call->PC, Lookup->Result);
      EXPECT_EQ(Call->Library, "libfinalize-model.so");
    }
  }
}
TEST_P(AndroidFinalizers, FiniteRegistryRejectsBeforeSuccessAndCanBeReused) {
  Options.Android->Initialize = false;
  Options.Android->TraceLimit = 0;
  Options.InstructionQuantum = 1000;
  Options.Limits.Instructions = 1000000;
  Options.Limits.Events = 20000;
  Options.Limits.TimeoutMicroseconds = 30000000;
  returned(run("cxa_capacity", {4096, 2}));
  auto R = run("cxa_capacity", {4097, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  EXPECT_EQ(R.Diagnostic,
            "Android native: exit callback registry limit exceeded");
  ASSERT_EQ(R.NativeCalls.size(), 4097u);
  EXPECT_EQ(R.NativeCalls[4095].Result, 0u);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
INSTANTIATE_TEST_SUITE_P(
    OptimizationAndPacking, AndroidFinalizers,
    testing::Values("finalizers-O0-none", "finalizers-O0-android",
                    "finalizers-O0-relr", "finalizers-O2-none",
                    "finalizers-O2-android", "finalizers-O2-relr"));
} // namespace
} // namespace neverd::emulation
