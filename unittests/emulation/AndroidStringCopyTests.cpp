//===- AndroidStringCopyTests.cpp - Bounded copy boundaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks exact guest bytes, bounded reads and terminal refusal semantics.
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
constexpr uint64_t Source = Buffer + 4096;
constexpr uint64_t End = Source + 4096;

class AndroidStringCopy : public testing::TestWithParam<
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
    Options.Android->TraceLimit = 8192;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory = {{Buffer, 8192}};
    Options.InstructionQuantum = 7;
#endif
  }

  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }

  void returned(const ProcessResult &R, uint64_t Value) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
  }

  void rejected(const ProcessResult &R, const char *Name,
                llvm::StringRef Diagnostic) {
    ASSERT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find(Diagnostic.str()), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Name);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_TRUE(R.MemorySnapshots.empty());
  }
};

TEST_P(AndroidStringCopy, ExactBytesPaddingTruncationReturnAndErrno) {
  for (uint64_t Count : {0u, 1u, 3u, 4u, 7u, 1024u}) {
    SCOPED_TRACE(Count);
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(8192, 0x5a);
    const std::array<uint8_t, 5> Text = {0x80, 0xff, 'x', 0, 'q'};
    std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
    auto Expected = Bytes;
    const std::array<uint8_t, 3> Prefix = {0x80, 0xff, 'x'};
    for (size_t I = 0; I < Count; ++I)
      Expected[16 + I] = I < Prefix.size() ? Prefix[I] : 0;
    llvm::support::endian::write32le(Expected.data() + 2048, 733);
    auto R = run("copy_supplied", {Buffer + 16, Source, Count, Buffer + 2048});
    returned(R, Buffer + 16);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    EXPECT_TRUE(R.Services.empty());
  }
}

TEST_P(AndroidStringCopy, SourceStopsAtNulOrCountEvenAtReadOnlyPageEnd) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[8189] = 'u';
  Bytes[8190] = 'v';
  Bytes[8191] = 0;
  auto Expected = Bytes;
  std::fill(Expected.begin() + 16, Expected.begin() + 26, 0);
  Expected[16] = 'u';
  Expected[17] = 'v';
  auto R = run("copy_readonly", {Source, Buffer + 16, End - 3, 10});
  returned(R, Buffer + 16);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);

  Bytes[8191] = 'w';
  Expected = Bytes;
  Expected[16] = 'v';
  Expected[17] = 'w';
  R = run("copy_readonly", {Source, Buffer + 16, End - 2, 2});
  returned(R, Buffer + 16);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  rejected(run("copy_readonly", {Source, Buffer + 16, End - 2, 3}), "strncpy",
           "invalid guest pointer");
}

TEST_P(AndroidStringCopy,
       DestinationCanSpanPagesAndPartialWriteFaultIsRejected) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[128] = 'p';
  Bytes[129] = 0;
  auto Expected = Bytes;
  std::fill(Expected.begin() + 4094, Expected.begin() + 4102, 0);
  Expected[4094] = 'p';
  auto R = run("copy_raw", {Source - 2, Buffer + 128, 8});
  returned(R, Source - 2);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  rejected(run("copy_readonly", {Source, Source - 2, Buffer + 128, 8}),
           "strncpy", "invalid guest pointer");
}

TEST_P(AndroidStringCopy, ZeroCountDoesNotAccessEitherPointer) {
  for (auto Address : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
    auto R = run("copy_raw", {Address, UINT64_MAX, 0});
    returned(R, Address);
  }
}

TEST_P(AndroidStringCopy, DestinationBoundsPermissionsAndFullWidthCount) {
  Options.Android->Memory[0].Bytes.assign(8192, 0);
  rejected(run("copy_readonly", {Buffer, Buffer, Source, 1}), "strncpy",
           "invalid guest pointer");
  rejected(run("copy_raw", {End - 1, Buffer, 2}), "strncpy",
           "invalid guest pointer");
  rejected(run("copy_raw", {UINT64_MAX, Buffer, 2}), "strncpy",
           "invalid guest pointer");
  rejected(run("copy_raw", {Buffer, UINT64_MAX, 1}), "strncpy",
           "invalid guest pointer");
  for (uint64_t Count : {UINT64_MAX, uint64_t(0x100000001)})
    rejected(run("copy_raw", {Buffer, Source, Count}), "strncpy",
             "memory limit");
}

TEST_P(AndroidStringCopy, UndefinedOverlapIncludesPaddingButNotUnreadSource) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[128] = 'a';
  Bytes[129] = 'b';
  Bytes[130] = 'c';
  Bytes[131] = 0;
  for (auto Destination : {Buffer + 128, Buffer + 129, Buffer + 127})
    rejected(run("copy_raw", {Destination, Buffer + 128, 4}), "strncpy",
             "overlapping strncpy");
  rejected(run("copy_raw", {Buffer + 112, Buffer + 128, 32}), "strncpy",
           "overlapping strncpy");
  // Disjoint accessed ranges in one mapped allocation are allowed.
  auto Expected = Bytes;
  const std::array<uint8_t, 8> Copy = {'a', 'b', 'c', 0, 0, 0, 0, 0};
  std::copy(Copy.begin(), Copy.end(), Expected.begin() + 132);
  auto R = run("copy_raw", {Buffer + 132, Buffer + 128, 8});
  returned(R, Buffer + 132);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidStringCopy, DynamicProviderIdentityAndClosedLibrary) {
  Options.Android->Libraries = {{"libcopy.so", {"strncpy"}}};
  Options.Android->Memory[0].Bytes.assign(8192, 0x5a);
  auto R = run("copy_dynamic", {Buffer, 0});
  returned(R, Buffer);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  const std::vector<uint8_t> Expected = {'a', 'b', 0, 0, 0, 0, 0x5a};
  EXPECT_EQ(std::vector<uint8_t>(R.MemorySnapshots[0].Bytes.begin(),
                                 R.MemorySnapshots[0].Bytes.begin() + 7),
            Expected);
  unsigned Calls = 0;
  uint64_t Target = 0;
  for (const auto &C : R.NativeCalls) {
    if (C.Name == "dlsym") {
      EXPECT_EQ(C.Symbol, "strncpy");
      ASSERT_TRUE(C.Result);
      Target = *C.Result;
    }
    if (C.Name == "strncpy") {
      EXPECT_EQ(C.Library, "libcopy.so");
      EXPECT_EQ(C.PC, Target);
      EXPECT_EQ(C.Result, Buffer);
      ++Calls;
    }
  }
  EXPECT_EQ(Calls, 1u);
  R = run("copy_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strncpy");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}

INSTANTIATE_TEST_SUITE_P(
    API28, AndroidStringCopy,
    testing::Combine(testing::Values("copy-O0-none", "copy-O0-android",
                                     "copy-O0-relr", "copy-O2-none",
                                     "copy-O2-android", "copy-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));

TEST(AndroidStringCopyMemory, RefusalsAreAtomicAndBudgetIsNotTruncated) {
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
  llvm::cantFail(Space.map(Buffer, 8192, Read | Write | UserAccessible));
  std::vector<uint8_t> Before(8192, 'x');
  Before[4096] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 16;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, End, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    uint64_t Destination, Input, Count;
    const char *Error;
  };
  for (auto C : {Case{Buffer, Source, 17, "memory limit"},
                 {Buffer, Source, 0x100000001, "memory limit"},
                 {End - 1, Source, 2, "invalid guest pointer"},
                 {Buffer, End - 1, 2, "invalid guest pointer"},
                 {Buffer, Buffer + 1, 16, "overlapping strncpy"}}) {
    SCOPED_TRACE(C.Count);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "strncpy";
    Call.Arguments = {C.Destination, C.Input, C.Count};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
              std::string::npos);
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
  // A writable prefix followed by a read-only page cannot be half-published.
  llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
  {
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = "strncpy";
    Call.Arguments = {Source - 1, Buffer, 2};
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find("invalid guest pointer"),
              std::string::npos);
    std::vector<uint8_t> After(8192);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
    // The exact limit is admitted and the read-only source NUL needs one byte.
    Call.Arguments = {Buffer, Source, 16};
    Value = Model.invoke(Call);
    ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
    ASSERT_TRUE(*Value);
    EXPECT_EQ(std::get<uint64_t>(**Value), Buffer);
    std::fill(Before.begin(), Before.begin() + 16, 0);
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
  auto Budget = llvm::cantFail(ExecutionBudget::create(
      {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                              Linked);
  NativeCallEvent Call{};
  Call.Name = "strncpy";
  Call.Arguments = {Buffer, Source, 16};
  auto Value = Model.invoke(Call);
  ASSERT_FALSE(bool(Value));
  EXPECT_NE(llvm::toString(Value.takeError()).find("timed out"),
            std::string::npos);
  EXPECT_TRUE(Model.timedOut());
  std::vector<uint8_t> After(8192);
  llvm::cantFail(Space.snapshotBacking(Buffer, After));
  EXPECT_EQ(After, Before);
}
} // namespace
} // namespace neverd::emulation
