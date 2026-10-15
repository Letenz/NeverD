//===- AndroidPropertyTests.cpp - Independent property API workloads -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check guest property enumeration and callbacks with independent C callers.

#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidProperties : public testing::TestWithParam<const char *> {
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
    Options.Android->Initialize = false;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory = {{Buffer, 512}};
    Options.Android->Properties = {
        {"test.alpha", "one"}, {"test.empty", ""}, {"test.last", "second"}};
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
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
    for (const auto &Call : R.NativeCalls)
      EXPECT_TRUE(Call.Result) << Call.Name;
  }
  void memory(const ProcessResult &R, llvm::ArrayRef<uint64_t> Words) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(512);
    for (size_t I = 0; I < Words.size(); ++I)
      llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  void input(llvm::StringRef Key, uint64_t Offset = 1024) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    if (Bytes.empty())
      Bytes.assign(8192, 0xa5);
    std::copy(Key.begin(), Key.end(), Bytes.begin() + Offset);
    Bytes[Offset + Key.size()] = 0;
  }
  void incomplete(const ProcessResult &R) {
    EXPECT_FALSE(R.ReturnValue);
    bool Found = false;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name != "__system_property_foreach")
        continue;
      Found = true;
      EXPECT_FALSE(Call.Result);
    }
    EXPECT_TRUE(Found);
  }
};

TEST_P(AndroidProperties, EnumeratesGuestCallbacksWithStableHandlesAndErrno) {
  auto R = run("property_sequence", {Buffer});
  returned(R, 3);
  memory(R, {3,
             0,
             1,
             1,
             719,
             0x123456789abcdef0,
             0xfedcba9876543210,
             0,
             3,
             0xece42d7a4bc366ee,
             0x1d4996,
             2,
             0xece42d7a4bc366ee,
             0x1d4996,
             0x3000000,
             0x3000000,
             0,
             0xece42d7a921c9409,
             0,
             2,
             0xece42d7a921c9409,
             0,
             0,
             0,
             6,
             0x8c8e71b0d12fa58a,
             0x40ff6b81f24,
             2,
             0x8c8e71b0d12fa58a,
             0x40ff6b81f24,
             0x6000000,
             0x6000000});
  EXPECT_TRUE(R.Services.empty());
  ASSERT_GE(R.NativeCalls.size(), 3u);
  EXPECT_EQ(R.NativeCalls[1].Name, "__system_property_foreach");
  EXPECT_EQ(R.NativeCalls[1].Result, 0u);
}

TEST_P(AndroidProperties, EmptyCatalogueDoesNotInvokeOrInspectCallback) {
  Options.Android->Properties.clear();
  returned(run("property_foreach_supplied", {1, UINT64_MAX}), 0);
  returned(run("property_find_nth", {0}), 0);
  input("test.alpha");
  returned(run("property_find_supplied", {Buffer + 1024}), 0);
  auto R = run("property_get_supplied", {Buffer + 1024, Buffer});
  returned(R, 0);
  std::vector<uint8_t> Expected(512, 0xa5);
  Expected[0] = 0;
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidProperties, LongNamesAreTruncatedOnlyByLegacyRead) {
  const std::string Key = "test." + std::string(60, 'a');
  Options.Android->Properties = {{Key, "one"}};
  input(Key);
  auto R = run("property_read_supplied", {Buffer + 1024, Buffer, Buffer + 64});
  returned(R, 3);
  std::vector<uint8_t> Expected(512, 0xa5);
  std::copy_n(Key.begin(), 31, Expected.begin());
  Expected[31] = 0;
  std::copy_n("one", 4, Expected.begin() + 64);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  auto &Bytes = Options.Android->Memory[0].Bytes;
  std::fill_n(Bytes.begin(), 512, 0);
  R = run("property_callback_supplied", {Buffer + 1024, Buffer});
  returned(R, 0);
  memory(R, {1, 0x0f8c36c5aa5d6b48, 0x1d4996, 0x3000000, 0x3000000});
}

TEST_P(AndroidProperties, LegacyReadUsesExactBytesAndValueThenNameOrder) {
  for (const std::string Value :
       {std::string(), std::string("one"), std::string(91, 'v')}) {
    Options.Android->Properties = {{"test.alpha", Value}};
    input("test.alpha");
    for (uint64_t Name : {uint64_t(0), Buffer, Buffer + 2}) {
      auto R = run("property_read_supplied", {Buffer + 1024, Name, Buffer});
      returned(R, Value.size());
      std::vector<uint8_t> Expected(512, 0xa5);
      std::copy(Value.begin(), Value.end(), Expected.begin());
      Expected[Value.size()] = 0;
      if (Name)
        std::copy_n("test.alpha", 11, Expected.begin() + Name - Buffer);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidProperties, LookupStopsAtTerminatorAndNthUsesUnsignedLowWord) {
  input("test.alpha", 8192 - 11);
  auto R = run("property_find_supplied", {Buffer + 8192 - 11});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  ASSERT_TRUE(R.ReturnValue);
  const uint64_t Handle = *R.ReturnValue;
  EXPECT_NE(Handle, 0u);
  for (uint64_t Index : {uint64_t(0), uint64_t(1) << 32}) {
    R = run("property_find_nth", {Index});
    returned(R, Handle);
    ASSERT_EQ(R.NativeCalls.size(), 1u);
    EXPECT_EQ(R.NativeCalls[0].Arguments[0], Index);
  }
  returned(run("property_find_nth", {UINT64_MAX}), 0);
  returned(run("property_serial_handle", {Handle}), 0x3000000);
  Options.Android->Properties.clear();
  R = run("property_serial_handle", {Handle});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(R.Diagnostic.find("invalid Android property handle"),
            std::string::npos);
}

TEST_P(AndroidProperties, InvalidHandlesAndInaccessibleOutputsCannotSucceed) {
  input("test.alpha");
  auto R = run("property_find_supplied", {Buffer + 1024});
  ASSERT_TRUE(R.ReturnValue);
  for (uint64_t Handle :
       {uint64_t(0), uint64_t(1), Buffer, *R.ReturnValue + 1, UINT64_MAX}) {
    auto Failed = run("property_read_handle", {Handle, Buffer, Buffer + 64});
    EXPECT_EQ(Failed.Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_NE(Failed.Diagnostic.find("invalid Android property handle"),
              std::string::npos);
    EXPECT_FALSE(Failed.NativeCalls.back().Result);
  }
  for (uint64_t Output : {uint64_t(0), Buffer + 8190, UINT64_MAX}) {
    R = run("property_read_supplied", {Buffer + 1024, 0, Output});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
  R = run("property_read_protected", {Buffer + 1024, Buffer, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

TEST_P(AndroidProperties,
       NestedEnumerationAndOnceRetainIndependentContinuations) {
  auto R = run("property_nested", {Buffer});
  returned(R, 0);
  memory(R, {3, 0, 3, 0x9000000, 9, 0x1b000000, 0, 0, 3, 0x8c8e71b0d12fa58a,
             0x40ff6b81f24, 0x6000000, 0x6000000});
}

TEST_P(AndroidProperties, CallbackCookieRetainsEveryBitAndGuestErrnoChanges) {
  returned(run("property_cookie"), 811);
}

TEST_P(AndroidProperties,
       ThreadsSharePropertyIdentityWithSeparateCallbackStacks) {
  Options.Android->ThreadLimit = 2;
  auto R = run("property_threads", {Buffer});
  returned(R, 3);
  memory(R,
         {3, 0x9000000,          0,        0,         0,         0, 0,   0,
          1, 0xece42d7a4bc366ee, 0x1d4996, 0x3000000, 0x3000000, 0, 101, 101,
          3, 0x9000000,          0,        0,         0,         0, 0,   0,
          1, 0xece42d7a4bc366ee, 0x1d4996, 0x3000000, 0x3000000, 0, 202, 202});
}

TEST_P(AndroidProperties, DynamicCallsRetainProviderIdentityAndLifetime) {
  Options.Android->Libraries["libproperties.so"] = {
      "__system_property_find", "__system_property_read",
      "__system_property_read_callback", "__system_property_foreach"};
  for (bool Resident : {false, true}) {
    if (Resident)
      Options.Android->DefaultScope = {"libproperties.so"};
    for (bool Close : {false, true}) {
      auto R = run("property_dynamic", {Buffer, Close});
      if (Close && !Resident) {
        EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
        ASSERT_FALSE(R.NativeCalls.empty());
        EXPECT_EQ(R.NativeCalls.back().Name, "__system_property_read");
        EXPECT_FALSE(R.NativeCalls.back().Result);
        continue;
      }
      returned(R, 0);
      memory(R, {1, 0xece42d7a4bc366ee, 0x1d4996, 0x3000000, 0x3000000, 3,
                 0x1d4996, 1, 3, 0x9000000});
      size_t Named = 0;
      for (const auto &Call : R.NativeCalls)
        if (Call.Name != "dlsym" && Call.Name != "dlopen" &&
            Call.Name != "dlclose" && Call.Library == "libproperties.so")
          ++Named;
      EXPECT_EQ(Named, 4u);
    }
  }
}

TEST_P(AndroidProperties, InvalidCallbackAddressesLeaveParentCallsIncomplete) {
  input("test.alpha");
  for (uint64_t Entry : {uint64_t(0), uint64_t(1), Buffer, UINT64_MAX}) {
    auto R = run("property_foreach_supplied", {Entry, UINT64_MAX});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    incomplete(R);
    R = run("property_callback_address", {Buffer + 1024, Entry, UINT64_MAX});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_EQ(R.NativeCalls.back().Name, "__system_property_read_callback");
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

TEST_P(AndroidProperties,
       UnsupportedCallbacksAndBudgetsDoNotCompleteEnumeration) {
  auto R = run("property_stop", {0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  incomplete(R);
  EXPECT_EQ(R.NativeCalls.back().Name, "unknown_property_callback");
  Options.Limits.Instructions = 1000;
  for (uint64_t Mode : {1u, 2u}) {
    R = run("property_stop", {Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
    EXPECT_EQ(R.Instructions, 1000u);
    incomplete(R);
  }
  Options.Limits.Instructions = 100000;
  Options.Limits.Events = 5;
  R = run("property_stop", {2});
  EXPECT_EQ(R.Stop, ProcessStopReason::EventLimit);
  incomplete(R);
}

TEST_P(AndroidProperties, CallbackMustReturnWithItsSuspendedStack) {
  auto R = run("property_unbalanced");
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(R.Diagnostic.find("stack"), std::string::npos);
  incomplete(R);
}

TEST_P(AndroidProperties,
       PropertyPagesAreReadOnlyAndTamperingCannotUseCachedValues) {
  input("test.alpha");
  for (uint64_t Mode : {0u, 1u, 2u}) {
    auto R = run("property_tamper", {Buffer + 1024, Mode});
    EXPECT_NE(R.Stop, ProcessStopReason::Returned);
    EXPECT_FALSE(R.ReturnValue);
    if (Mode == 2) {
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
      EXPECT_NE(R.Diagnostic.find("changed the immutable Android property"),
                std::string::npos);
      EXPECT_FALSE(R.NativeCalls.back().Result);
    }
  }
}

TEST_P(AndroidProperties, OutputCannotMutatePropertyInputWithinTheSameCall) {
  input("test.alpha");
  for (uint64_t Get : {0u, 1u}) {
    auto R = run("property_read_image_output", {Buffer + 1024, Get});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_NE(R.Diagnostic.find("property output aliases"), std::string::npos);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

TEST_P(AndroidProperties, NativeTransportMatchesCallbackAndThreadEffects) {
  ExecutionConfiguration Configuration;
  Configuration.Backend = ExecutionBackendKind::HVF;
  Configuration.Architecture = GuestArchitecture::AArch64;
  Configuration.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  Options.Android->ThreadLimit = 2;
  for (const char *Entry : {"property_nested", "property_threads"}) {
    Options.Backend = ExecutionBackendKind::Unicorn;
    auto Software = run(Entry, {Buffer});
    Options.Backend = ExecutionBackendKind::HVF;
    auto Native = run(Entry, {Buffer});
    ASSERT_EQ(Software.Stop, ProcessStopReason::Returned)
        << Software.Diagnostic;
    ASSERT_EQ(Native.Stop, Software.Stop) << Native.Diagnostic;
    EXPECT_EQ(Native.SelectedBackend, ExecutionBackendKind::HVF);
    EXPECT_EQ(Native.ReturnValue, Software.ReturnValue);
    ASSERT_EQ(Native.MemorySnapshots.size(), 1u);
    ASSERT_EQ(Software.MemorySnapshots.size(), 1u);
    EXPECT_EQ(Native.MemorySnapshots[0].Bytes,
              Software.MemorySnapshots[0].Bytes);
    ASSERT_EQ(Native.NativeCalls.size(), Software.NativeCalls.size());
    for (size_t I = 0; I < Native.NativeCalls.size(); ++I) {
      EXPECT_EQ(Native.NativeCalls[I].Name, Software.NativeCalls[I].Name);
      EXPECT_EQ(Native.NativeCalls[I].Arguments,
                Software.NativeCalls[I].Arguments);
      EXPECT_EQ(Native.NativeCalls[I].Result, Software.NativeCalls[I].Result);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    PackedAndOptimization, AndroidProperties,
    testing::Values("properties-O0-none", "properties-O0-android",
                    "properties-O0-relr", "properties-O2-none",
                    "properties-O2-android", "properties-O2-relr"));

TEST(AndroidPropertyImage, AggregateLimitAndMalformedInputsDoNotPublishMemory) {
  auto Physical = PhysicalMemory::create(8192);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, 8192);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 4096;
  auto Budget = ExecutionBudget::create(Options.Limits);
  ASSERT_TRUE(bool(Budget)) << llvm::toString(Budget.takeError());
  for (const auto &Properties :
       {std::map<std::string, std::string>{{"", "value"}},
        {{std::string("nul\0name", 8), "value"}},
        {{"test.alpha", std::string("nul\0value", 9)}},
        {{"test.alpha", std::string(92, 'v')}},
        {{std::string(2000, 'a'), "a"}, {std::string(2000, 'b'), "b"}}}) {
    Options.Android->Properties = Properties;
    auto Table =
        android_model::initializeProperties(**Space, Options, **Budget);
    ASSERT_FALSE(bool(Table));
    llvm::consumeError(Table.takeError());
    EXPECT_EQ((*Space)->mappedBytes(), 0u);
  }
  Options.Android->Properties = {{"test.alpha", "one"}};
  auto Table = android_model::initializeProperties(**Space, Options, **Budget);
  ASSERT_TRUE(bool(Table)) << llvm::toString(Table.takeError());
  EXPECT_EQ((*Space)->mappedBytes(), 4096u);
  ASSERT_EQ(Table->Records.size(), 1u);
  std::vector<uint8_t> Bytes(107);
  ASSERT_EQ(llvm::toString(
                (*Space)->snapshotBacking(Table->Records[0].Address, Bytes)),
            "");
  // Independently specified short prop_info layout, including full name NUL.
  std::vector<uint8_t> Expected(107);
  Expected[3] = 3;
  std::copy_n("one", 4, Expected.begin() + 4);
  std::copy_n("test.alpha", 11, Expected.begin() + 96);
  EXPECT_EQ(Bytes, Expected);
}
} // namespace
} // namespace neverd::emulation
