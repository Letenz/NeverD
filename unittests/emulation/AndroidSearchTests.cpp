//===- AndroidSearchTests.cpp - Guest character-search boundaries --------===//
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
constexpr uint64_t PageEnd = Buffer + 4096;
constexpr std::array<const char *, 4> SearchNames = {
    "strchr", "strrchr", "__strchr_chk", "__strrchr_chk"};

class AndroidSearch : public testing::TestWithParam<
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
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory = {{Buffer, 128}, {PageEnd - 16, 16}};
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

TEST_P(AndroidSearch, FirstLastNulUnsignedCharactersAndFullWidthBounds) {
  auto R = run("search_sequence", {Buffer});
  returned(R, 0);
  const std::array<uint64_t, 16> Expected = {2, 4, 2, 4, 1, 5, 6,   6,
                                             0, 0, 0, 0, 2, 4, 733, 1};
  ASSERT_EQ(R.MemorySnapshots.size(), 2u);
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read64le(
                  R.MemorySnapshots[0].Bytes.data() + 8 * I),
              Expected[I]);
  EXPECT_TRUE(R.Services.empty());
}

TEST_P(AndroidSearch, PageTailMatchDoesNotReadBeyondTheObject) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  for (auto Character : {uint8_t(':'), uint8_t(0), uint8_t(0xff)}) {
    SCOPED_TRACE(unsigned(Character));
    Bytes.assign(4096, 'x');
    Bytes.back() = Character;
    for (unsigned Mode : {0u, 2u}) {
      SCOPED_TRACE(Mode);
      auto R = run(
          "search_readonly",
          {Buffer, PageEnd - 1, 0xabcdef0100ULL | Character, UINT64_MAX, Mode});
      returned(R, PageEnd - 1);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(R.MemorySnapshots[1].Bytes,
                std::vector<uint8_t>(Bytes.end() - 16, Bytes.end()));
    }
  }
  for (unsigned Mode : {1u, 3u}) {
    auto R = run("search_supplied", {PageEnd - 1, 0, 1, Mode});
    // Reverse search cannot use a non-NUL match as a substitute for
    // termination.
    rejected(R, SearchNames[Mode],
             Mode == 3 ? "FORTIFY" : "invalid guest pointer");
  }
}

TEST_P(AndroidSearch, TerminatorAtTheLastByteIsIncludedInTheSearch) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes.assign(4096, 'x');
  Bytes[4092] = ':';
  Bytes[4094] = ':';
  Bytes[4095] = 0;
  for (unsigned Mode = 0; Mode < SearchNames.size(); ++Mode) {
    for (auto [Character, Expected] :
         {std::pair{uint64_t(':'), Mode % 2 ? PageEnd - 2 : PageEnd - 4},
          {0x100, PageEnd - 1},
          {'z', 0}}) {
      SCOPED_TRACE(SearchNames[Mode]);
      auto R =
          run("search_readonly", {Buffer, PageEnd - 4, Character, 4, Mode});
      returned(R, Expected);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(R.MemorySnapshots[1].Bytes,
                std::vector<uint8_t>(Bytes.end() - 16, Bytes.end()));
    }
  }
}

TEST_P(AndroidSearch, FortifyChecksExtentBeforePointerOrNextCharacter) {
  Options.Android->Memory[0].Bytes.assign(4096, 'x');
  for (unsigned Mode : {2u, 3u}) {
    for (uint64_t Address : {uint64_t(0), uint64_t(1), UINT64_MAX, PageEnd - 1})
      rejected(run("search_supplied", {Address, 'x', 0, Mode}),
               SearchNames[Mode], "FORTIFY");
    // The inaccessible next byte must never be read after exhausting the bound.
    rejected(run("search_supplied", {PageEnd - 1, 'z', 1, Mode}),
             SearchNames[Mode], "FORTIFY");
    // Reading a still-in-bounds inaccessible byte remains a pointer failure.
    rejected(run("search_supplied", {PageEnd - 1, 'z', 2, Mode}),
             SearchNames[Mode], "invalid guest pointer");
  }
  rejected(run("search_supplied", {PageEnd - 1, 'x', 1, 3}), "__strrchr_chk",
           "FORTIFY");
  returned(run("search_supplied", {PageEnd - 1, 'x', 1, 2}), PageEnd - 1);
}

TEST_P(AndroidSearch, BoundsExcludeAnOtherwiseAccessibleTerminator) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  Bytes = {'x', ':', 'x', 0};
  returned(run("search_supplied", {Buffer, ':', 2, 2}), Buffer + 1);
  for (unsigned Mode : {2u, 3u}) {
    rejected(run("search_supplied", {Buffer, 0, 3, Mode}), SearchNames[Mode],
             "FORTIFY");
    rejected(run("search_supplied", {Buffer, 'z', 3, Mode}), SearchNames[Mode],
             "FORTIFY");
    returned(run("search_supplied", {Buffer, 0, 4, Mode}), Buffer + 3);
    returned(run("search_supplied", {Buffer, 'z', 4, Mode}), 0);
  }
}

TEST_P(AndroidSearch, NamedCallsRetainProviderAndRejectClosedLibrary) {
  Options.Android->Libraries = {
      {"libsearch.so", {"__strchr_chk", "__strrchr_chk"}}};
  for (unsigned Reverse : {0u, 1u}) {
    const char *Name = SearchNames[Reverse + 2];
    auto R = run("search_dynamic", {Buffer, 0, Reverse});
    returned(R, 0);
    uint64_t Target = 0;
    unsigned Count = 0;
    for (const auto &Call : R.NativeCalls) {
      if (Call.Name == "dlsym") {
        EXPECT_EQ(Call.Library, "libsearch.so");
        EXPECT_EQ(Call.Symbol, Name);
        ASSERT_TRUE(Call.Result);
        Target = *Call.Result;
      }
      if (Call.Name == Name) {
        EXPECT_EQ(Call.Library, "libsearch.so");
        EXPECT_EQ(Call.PC, Target);
        EXPECT_TRUE(Call.Result);
        ++Count;
      }
    }
    EXPECT_NE(Target, 0u);
    EXPECT_EQ(Count, 2u);
    ASSERT_EQ(R.MemorySnapshots.size(), 2u);
    const std::array<uint64_t, 3> Expected = {Reverse ? 10u : 4u, 15, 733};
    for (size_t I = 0; I < Expected.size(); ++I)
      EXPECT_EQ(llvm::support::endian::read64le(
                    R.MemorySnapshots[0].Bytes.data() + 8 * I),
                Expected[I]);
    R = run("search_dynamic", {Buffer, 1, Reverse});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, Name);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidSearch,
    testing::Combine(testing::Values("search-O0-none", "search-O0-android",
                                     "search-O0-relr", "search-O2-none",
                                     "search-O2-android", "search-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));

TEST(AndroidSearchMemory, ObjectBoundsBudgetsAndFailuresPreserveGuestBytes) {
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
      llvm::toString(Space.map(Buffer, 4096, Read | Write | UserAccessible)),
      "");
  std::array<uint8_t, 16> Before{};
  Before.fill('x');
  Before[8] = 0;
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, Before)), "");
  ASSERT_EQ(llvm::toString(Space.protect(Buffer, 4096, Read | UserAccessible)),
            "");
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 8;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, PageEnd, Options, Result);
  const android_model::LinkedImage Linked{};
  struct Case {
    unsigned Mode;
    uint64_t Character, Bound;
    const char *Error;
  };
  for (auto C : {Case{2, 'z', 0, "FORTIFY"},
                 {2, 'z', 8, "FORTIFY"},
                 {3, 'x', 8, "FORTIFY"},
                 {2, 'z', 9, "scan exceeds memory limit"},
                 {3, 'x', UINT64_MAX, "scan exceeds memory limit"},
                 {0, 'z', 0, "scan exceeds memory limit"},
                 {1, 'x', 0, "scan exceeds memory limit"},
                 {2, 0xfedcba9876543278, UINT64_MAX, nullptr}}) {
    SCOPED_TRACE(C.Mode);
    SCOPED_TRACE(C.Bound);
    auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
    android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                                Linked);
    NativeCallEvent Call{};
    Call.Name = SearchNames[C.Mode];
    Call.Arguments = {Buffer, C.Character, C.Bound};
    auto Value = Model.invoke(Call);
    if (C.Error) {
      ASSERT_FALSE(bool(Value));
      EXPECT_NE(llvm::toString(Value.takeError()).find(C.Error),
                std::string::npos);
    } else {
      ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
      ASSERT_TRUE(*Value);
      EXPECT_EQ(std::get<uint64_t>(**Value), Buffer);
    }
    EXPECT_FALSE(Model.timedOut());
    std::array<uint8_t, 16> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
    EXPECT_EQ(After, Before);
  }
  auto Budget = llvm::cantFail(ExecutionBudget::create(
      {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                              Linked);
  NativeCallEvent Call{};
  Call.Name = "__strchr_chk";
  Call.Arguments = {Buffer, 'x', 1};
  auto Value = Model.invoke(Call);
  ASSERT_FALSE(bool(Value));
  EXPECT_NE(
      llvm::toString(Value.takeError()).find("native call model timed out"),
      std::string::npos);
  EXPECT_TRUE(Model.timedOut());
  std::array<uint8_t, 16> After{};
  ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
  EXPECT_EQ(After, Before);
}
} // namespace
} // namespace neverd::emulation
