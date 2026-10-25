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
constexpr std::array<const char *, 2> CheckedNames = {"__strncpy_chk",
                                                      "__strncpy_chk2"};

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

TEST_P(AndroidStringCopy, CheckedBytesPaddingTruncationAndErrno) {
  for (unsigned Mode : {0u, 1u}) {
    for (const auto [Count, SourceSize] : {std::pair{0u, 0u},
                                           {1u, 1u},
                                           {3u, 3u},
                                           {4u, 4u},
                                           {7u, 4u},
                                           {1024u, 4u}}) {
      SCOPED_TRACE(testing::Message() << Mode << ":" << Count);
      auto &Bytes = Options.Android->Memory[0].Bytes;
      Bytes.assign(8192, 0x5a);
      const std::array<uint8_t, 5> Text = {0x80, 0xff, 'x', 0, 'q'};
      std::copy(Text.begin(), Text.end(), Bytes.begin() + 4096);
      auto Expected = Bytes;
      const std::array<uint8_t, 3> Prefix = {0x80, 0xff, 'x'};
      for (size_t I = 0; I < Count; ++I)
        Expected[16 + I] = I < Prefix.size() ? Prefix[I] : 0;
      llvm::support::endian::write32le(Expected.data() + 2048, 733);
      auto R = run("copy_checked_supplied", {Buffer + 16, Source, Count, Count,
                                             SourceSize, Mode, Buffer + 2048});
      returned(R, Buffer + 16);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      EXPECT_TRUE(R.Services.empty());
      auto Call = std::find_if(
          R.NativeCalls.begin(), R.NativeCalls.end(),
          [&](const auto &C) { return C.Name == CheckedNames[Mode]; });
      ASSERT_NE(Call, R.NativeCalls.end());
      EXPECT_EQ(Call->Result, Buffer + 16);
      EXPECT_EQ(Call->Arguments[2], Count);
      EXPECT_EQ(Call->Arguments[3], Count);
      if (Mode)
        EXPECT_EQ(Call->Arguments[4], SourceSize);
    }
  }
}

TEST_P(AndroidStringCopy, CheckedDestinationExtentPrecedesMemoryAccess) {
  Options.Android->Memory[0].Bytes.assign(8192, 'x');
  for (unsigned Mode : {0u, 1u}) {
    for (uint64_t Count : {uint64_t(1), uint64_t(0x100000001), UINT64_MAX}) {
      auto R = run("copy_checked",
                   {UINT64_MAX, UINT64_MAX, Count, Count - 1, 0, Mode});
      rejected(R, CheckedNames[Mode], "FORTIFY destination extent exceeded");
    }
    // A source NUL cannot reduce the count written, because strncpy pads.
    Options.Android->Memory[0].Bytes[4096] = 0;
    rejected(
        run("copy_checked_readonly", {Buffer, Buffer, Source, 7, 1, 1, Mode}),
        CheckedNames[Mode], "FORTIFY destination extent exceeded");
  }
}

TEST_P(AndroidStringCopy, CheckedZeroCountAndFullWidthExtents) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[4096] = 'a';
  Bytes[4097] = 0;
  for (unsigned Mode : {0u, 1u}) {
    for (uint64_t Address : {uint64_t(0), uint64_t(1), UINT64_MAX}) {
      auto R = run("copy_checked", {Address, UINT64_MAX, 0, 0, 0, Mode});
      returned(R, Address);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
    }
    auto Expected = Bytes;
    Expected[16] = 'a';
    std::fill(Expected.begin() + 17, Expected.begin() + 23, 0);
    for (uint64_t Extent : {uint64_t(0x100000000), UINT64_MAX}) {
      auto R =
          run("copy_checked", {Buffer + 16, Source, 7, Extent, Extent, Mode});
      returned(R, Buffer + 16);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidStringCopy, CheckedSourceExtentCountsReadNulButNotPadding) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[4096] = 'a';
  Bytes[4097] = 'b';
  Bytes[4098] = 'c';
  Bytes[4099] = 0;
  // Truncation needs no terminator. Padding only needs the source through NUL.
  for (const auto [Count, SourceSize] :
       {std::pair{3u, 3u}, {4u, 4u}, {16u, 4u}}) {
    auto Expected = Bytes;
    std::fill(Expected.begin(), Expected.begin() + Count, 0);
    Expected[0] = 'a';
    Expected[1] = 'b';
    Expected[2] = 'c';
    auto R = run("copy_checked", {Buffer, Source, Count, Count, SourceSize, 1});
    returned(R, Buffer);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  for (uint64_t SourceSize : {0u, 2u, 3u})
    rejected(run("copy_checked", {Buffer, Source, 16, 16, SourceSize, 1}),
             "__strncpy_chk2", "FORTIFY read exceeds object bounds");
  rejected(run("copy_checked", {Buffer, Source + 3, 7, 7, 0, 1}),
           "__strncpy_chk2", "FORTIFY read exceeds object bounds");
  auto Expected = Bytes;
  std::fill(Expected.begin(), Expected.begin() + 7, 0);
  auto R = run("copy_checked", {Buffer, Source + 3, 7, 7, 1, 1});
  returned(R, Buffer);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  // The four-argument entry has no source-object check.
  R = run("copy_checked", {Buffer, Source + 3, 7, 7, 0, 0});
  returned(R, Buffer);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidStringCopy, CheckedSourceScanFaultPrecedesSourceFortify) {
  Options.Android->Memory[0].Bytes.assign(8192, 'x');
  for (uint64_t SourceSize : {0u, 1u}) {
    // The next byte is inaccessible, even though the source extent is
    // exhausted.
    rejected(run("copy_checked", {Buffer, End - 1, 2, 2, SourceSize, 1}),
             "__strncpy_chk2", "invalid guest pointer");
  }
  rejected(run("copy_checked", {Buffer, End - 1, 1, 1, 0, 1}), "__strncpy_chk2",
           "FORTIFY read exceeds object bounds");
  rejected(run("copy_checked", {Buffer, UINT64_MAX, 1, 1, 0, 1}),
           "__strncpy_chk2", "invalid guest pointer");
}

TEST_P(AndroidStringCopy, CheckedPageTailReadOnlyInputsAndDestinationFaults) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[8189] = 'u';
  Bytes[8190] = 'v';
  Bytes[8191] = 0;
  for (unsigned Mode : {0u, 1u}) {
    auto Expected = Bytes;
    std::fill(Expected.begin() + 16, Expected.begin() + 26, 0);
    Expected[16] = 'u';
    Expected[17] = 'v';
    auto R = run("copy_checked_readonly",
                 {Source, Buffer + 16, End - 3, 10, 10, 3, Mode});
    returned(R, Buffer + 16);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    rejected(run("copy_checked_readonly",
                 {Source, Source - 1, Buffer, 2, 2, 2, Mode}),
             CheckedNames[Mode], "invalid guest pointer");
    rejected(
        run("copy_checked_readonly", {Buffer, Buffer, End - 1, 1, 1, 1, Mode}),
        CheckedNames[Mode], "invalid guest pointer");
  }
}

TEST_P(AndroidStringCopy, CheckedDynamicProviderIdentityAndLifetime) {
  Options.Android->Libraries = {
      {"libcopy-checked.so", {"__strncpy_chk", "__strncpy_chk2"}}};
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(8192, 0x5a);
  Bytes[4096] = 'a';
  Bytes[4097] = 0;
  auto Expected = Bytes;
  Expected[0] = 'a';
  std::fill(Expected.begin() + 1, Expected.begin() + 7, 0);
  for (unsigned Mode : {0u, 1u}) {
    auto R = run("copy_checked_dynamic", {Buffer, Source, 7, 7, 2, Mode, 0});
    returned(R, Buffer);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    uint64_t Target = 0;
    unsigned Calls = 0;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "dlsym") {
        EXPECT_EQ(Call.Symbol, CheckedNames[Mode]);
        ASSERT_TRUE(Call.Result);
        Target = *Call.Result;
      }
      if (Call.Name == CheckedNames[Mode]) {
        EXPECT_EQ(Call.Library, "libcopy-checked.so");
        EXPECT_EQ(Call.PC, Target);
        EXPECT_EQ(Call.Result, Buffer);
        ++Calls;
      }
    }
    EXPECT_NE(Target, 0u);
    EXPECT_EQ(Calls, 1u);
    rejected(run("copy_checked_dynamic", {Buffer, Source, 7, 6, 2, Mode, 0}),
             CheckedNames[Mode], "FORTIFY destination extent exceeded");
    R = run("copy_checked_dynamic", {UINT64_MAX, UINT64_MAX, 1, 0, 0, Mode, 1});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, CheckedNames[Mode]);
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_FALSE(R.ReturnValue);
  }
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
TEST(AndroidStringCopyMemory, CheckedBoundsAndRefusalsAreAtomic) {
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
  Before[4099] = 0;
  llvm::cantFail(CPU.write(Buffer, Before));
  llvm::cantFail(Space.protect(Source, 4096, Read | UserAccessible));
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
    unsigned Mode;
    uint64_t Destination, Input, Count, DestinationSize, SourceSize;
    bool Expired, TimesOut;
    const char *Error;
  };
  for (const auto C : {
           Case{0, Buffer, Source, 17, 17, UINT64_MAX, false, false,
                "memory limit"},
           {1, Buffer, Source, 17, 17, 0, false, false, "memory limit"},
           {0, UINT64_MAX, UINT64_MAX, 17, 16, 0, true, false,
            "FORTIFY destination"},
           {1, UINT64_MAX, UINT64_MAX, 17, 16, 0, true, false,
            "FORTIFY destination"},
           {0, Buffer, Source, 0x100000001, 1, 0, false, false,
            "FORTIFY destination"},
           {1, Buffer, Source, 0x100000001, 1, 0, false, false,
            "FORTIFY destination"},
           {0, Buffer, Source, 0x100000001, UINT64_MAX, UINT64_MAX, false,
            false, "memory limit"},
           {1, Buffer, Source, 0x100000001, UINT64_MAX, UINT64_MAX, false,
            false, "memory limit"},
           {0, Source - 1, Buffer, 2, 2, 2, false, false,
            "invalid guest pointer"},
           {1, Source - 1, Buffer, 2, 2, 0, false, false,
            "invalid guest pointer"},
           {0, Buffer, End - 1, 2, 2, 0, false, false, "invalid guest pointer"},
           {1, Buffer, End - 1, 2, 2, 0, false, false, "invalid guest pointer"},
           {0, Buffer, Buffer + 1, 4, 4, 4, false, false,
            "overlapping strncpy"},
           {1, Buffer, Buffer + 1, 4, 4, 4, false, false,
            "overlapping strncpy"},
           {1, Buffer, Source, 16, 16, 3, false, false, "FORTIFY read"},
           {1, Buffer, Source + 3, 16, 16, 0, false, false, "FORTIFY read"},
           {0, Buffer, Source, 4, 4, 0, true, true, "timed out"},
           {1, Buffer, Source, 4, 4, 0, true, true, "timed out"},
           {0, UINT64_MAX, UINT64_MAX, 0, 0, 0, true, false, nullptr},
           {1, UINT64_MAX, UINT64_MAX, 0, 0, 0, true, false, nullptr},
       }) {
    SCOPED_TRACE(testing::Message()
                 << C.Mode << ":" << C.Count << ":" << C.DestinationSize << ":"
                 << C.SourceSize << ":" << C.Expired);
    auto Budget = llvm::cantFail(ExecutionBudget::create(
        Options.Limits, C.Expired ? ExecutionBudget::Clock::time_point::min()
                                  : ExecutionBudget::Clock::now()));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = CheckedNames[C.Mode];
    Call.Arguments = {C.Destination, C.Input, C.Count, C.DestinationSize,
                      C.SourceSize};
    auto Value = Model.invoke(Call);
    if (C.Error) {
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
                std::string::npos);
    } else {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(*Value);
      EXPECT_EQ(std::get<uint64_t>(**Value), C.Destination);
    }
    EXPECT_EQ(Model.timedOut(), C.TimesOut);
    std::vector<uint8_t> After(Before.size());
    llvm::cantFail(Space.snapshotBacking(Buffer, After));
    EXPECT_EQ(After, Before);
  }
}
} // namespace
} // namespace neverd::emulation
