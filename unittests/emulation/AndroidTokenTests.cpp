//===- AndroidTokenTests.cpp - Independent guest tokenization workloads ---===//
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

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t Input = Buffer + 32, Delimiters = Buffer + 128;
constexpr uint64_t Saved = Buffer + 4096;
class AndroidToken : public testing::TestWithParam<const char *> {
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
    Options.Android->TraceLimit = 8192;
    Options.Android->Memory.push_back({Buffer, 8192, {}, false});
    Options.Android->ReadMemory = {{Buffer, 160}, {Saved, 16}};
    Options.InstructionQuantum = 7;
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
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
  }
  void input(llvm::StringRef Text, llvm::StringRef Delims,
             uint64_t Cursor = 0xabcdef0123456789) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    Bytes.assign(8192, 0xa5);
    for (auto [Offset, String] : {std::pair{32u, Text}, {128u, Delims}}) {
      std::copy(String.begin(), String.end(), Bytes.begin() + Offset);
      Bytes[Offset + String.size()] = 0;
    }
    llvm::support::endian::write64le(Bytes.data() + 4096, Cursor);
  }
};
TEST_P(AndroidToken, InterleavedContextsChangingDelimitersAndErrno) {
  auto R = run("token_sequence", {Buffer});
  returned(R, 0);
  const std::array<uint64_t, 16> Expected = {2,  6, 1, 5, 6, 12,  6, 10,
                                             12, 0, 0, 0, 0, 733, 1, 1};
  ASSERT_EQ(R.MemorySnapshots.size(), 2u);
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read64le(
                  R.MemorySnapshots[0].Bytes.data() + 8 * I),
              Expected[I]);
  size_t Calls = 0;
  for (const auto &Call : R.NativeCalls)
    if (Call.Name == "strtok_r") {
      ++Calls;
      EXPECT_TRUE(Call.Result);
    }
  EXPECT_EQ(Calls, 7u);
  EXPECT_TRUE(R.Services.empty());
}
TEST_P(AndroidToken, EmptyFinalAndUnsignedByteTokensRetainUntouchedBytes) {
  struct Case {
    const char *Text, *Delims;
    int Token, Next, Split;
  };
  for (auto C : {Case{"", ",", -1, -1, -1},
                 {",,,", ",", -1, -1, -1},
                 {"word", ",", 0, -1, -1},
                 {",one,two", ",,", 1, 5, 4},
                 {"abc", "", 0, -1, -1},
                 {"\x80x\xffy", "\x80\xff", 1, 3, 2},
                 {"x,", ",", 0, 2, 1}}) {
    SCOPED_TRACE(C.Text);
    input(C.Text, C.Delims);
    auto Before = Options.Android->Memory[0].Bytes;
    auto R = run("token_supplied", {Input, Delimiters, Saved});
    returned(R, C.Token < 0 ? 0 : Input + C.Token);
    ASSERT_EQ(R.MemorySnapshots.size(), 2u);
    if (C.Split >= 0)
      Before[32 + C.Split] = 0;
    EXPECT_EQ(R.MemorySnapshots[0].Bytes,
              std::vector<uint8_t>(Before.begin(), Before.begin() + 160));
    llvm::support::endian::write64le(Before.data() + 4096,
                                     C.Next < 0 ? 0 : Input + C.Next);
    EXPECT_EQ(
        R.MemorySnapshots[1].Bytes,
        std::vector<uint8_t>(Before.begin() + 4096, Before.begin() + 4112));
  }
}
TEST_P(AndroidToken, ResumeUsesGuestCursorAndExhaustionNeedsNoWrites) {
  input("..abc;tail", ".;", Input);
  auto R = run("token_supplied", {0, Delimiters, Saved});
  returned(R, Input + 2);
  ASSERT_EQ(R.MemorySnapshots.size(), 2u);
  EXPECT_EQ(llvm::support::endian::read64le(R.MemorySnapshots[1].Bytes.data()),
            Input + 6);
  input("ignored", ",", 0);
  R = run("token_protected", {0, 1, Saved, Saved});
  returned(R, 0);
  EXPECT_EQ(llvm::support::endian::read64le(R.MemorySnapshots[1].Bytes.data()),
            0u);
}
TEST_P(AndroidToken, ReadOnlyFinalTokenSucceedsButARequiredSplitFails) {
  for (const char *Text : {"word", ",,,", "one,two"}) {
    input(Text, ",");
    auto R = run("token_protected", {Input, Delimiters, Saved, Buffer});
    if (llvm::StringRef(Text) == "one,two") {
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
      EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_FALSE(R.NativeCalls.back().Result);
      EXPECT_TRUE(R.MemorySnapshots.empty());
      continue;
    } else {
      returned(R, llvm::StringRef(Text) == "word" ? Input : 0);
      ASSERT_EQ(R.MemorySnapshots.size(), 2u);
      EXPECT_EQ(
          llvm::support::endian::read64le(R.MemorySnapshots[1].Bytes.data()),
          0u);
    }
    EXPECT_EQ(
        R.MemorySnapshots[0].Bytes,
        std::vector<uint8_t>(Options.Android->Memory[0].Bytes.begin(),
                             Options.Android->Memory[0].Bytes.begin() + 160));
  }
}
TEST_P(AndroidToken, InvalidPointersAndDeniedCursorStopWithoutAResult) {
  for (auto Args : {std::vector<uint64_t>{1, Delimiters, Saved},
                    {Input, 1, Saved},
                    {Input, Delimiters, 8},
                    {Input, Delimiters, Saved + 1},
                    {0, Delimiters, 8}}) {
    input("one,two", ",");
    auto R = run("token_supplied", Args);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "strtok_r");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    EXPECT_TRUE(R.MemorySnapshots.empty());
  }
  input("one,two", ",");
  auto R = run("token_protected", {Input, Delimiters, Saved, Saved});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_FALSE(R.NativeCalls.back().Result);
  EXPECT_TRUE(R.MemorySnapshots.empty());
}
TEST_P(AndroidToken, NamedCallsRetainProviderAndRejectClosedLibrary) {
  Options.Android->Libraries = {{"libtokens.so", {"strtok_r"}}};
  auto R = run("token_dynamic", {Buffer, 0});
  returned(R, 0);
  uint64_t Target = 0;
  size_t Calls = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name == "dlsym") {
      EXPECT_EQ(Call.Library, "libtokens.so");
      EXPECT_EQ(Call.Symbol, "strtok_r");
      ASSERT_TRUE(Call.Result);
      Target = *Call.Result;
    }
    if (Call.Name == "strtok_r") {
      EXPECT_EQ(Call.Library, "libtokens.so");
      EXPECT_EQ(Call.PC, Target);
      ASSERT_TRUE(Call.Result);
      ++Calls;
    }
  }
  EXPECT_NE(Target, 0u);
  EXPECT_EQ(Calls, 2u);
  const std::array<uint64_t, 5> Expected = {0, 6, 0, 6, 0};
  for (size_t I = 0; I < Expected.size(); ++I)
    EXPECT_EQ(llvm::support::endian::read64le(
                  R.MemorySnapshots[0].Bytes.data() + I * 8),
              Expected[I]);
  R = run("token_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "strtok_r");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidToken,
                         testing::Values("token-O0-none", "token-O0-android",
                                         "token-O0-relr", "token-O2-none",
                                         "token-O2-android", "token-O2-relr"));

TEST(AndroidTokenMemory, ScanLimitsAndWriteDenialsPreserveBothObjects) {
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
  std::array<uint8_t, 160> Before{};
  std::array<uint8_t, 8> Cursor{};
  Cursor.fill(0xa5);
  ProcessOptions Options;
  Options.Android.emplace();
  Options.MemoryLimit = 8;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 8192, Options,
                                    Result);
  auto Budget = ExecutionBudget::create(Options.Limits);
  ASSERT_TRUE(bool(Budget)) << llvm::toString(Budget.takeError());
  const android_model::LinkedImage Linked{};
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, **Budget,
                              Linked);
  NativeCallEvent Call{};
  Call.Name = "strtok_r";
  Call.Arguments = {Input, Delimiters, Saved};
  const std::array<const char *, 4> Errors = {
      "scan exceeds memory limit", "unterminated guest string",
      "invalid guest pointer", "invalid guest pointer"};
  for (unsigned Case = 0; Case < Errors.size(); ++Case) {
    SCOPED_TRACE(Case);
    ASSERT_EQ(llvm::toString(
                  Space.protect(Buffer, 8192, Read | Write | UserAccessible)),
              "");
    Before.fill('x');
    Before[128] = ',';
    if (Case != 1)
      Before[129] = 0;
    if (Case >= 2)
      Before[33] = ',';
    ASSERT_EQ(llvm::toString(CPU.write(Buffer, Before)), "");
    ASSERT_EQ(llvm::toString(CPU.write(Saved, Cursor)), "");
    if (Case >= 2)
      ASSERT_EQ(llvm::toString(Space.protect(Case == 2 ? Saved : Buffer, 4096,
                                             Read | UserAccessible)),
                "");
    auto Value = Model.invoke(Call);
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Errors[Case]),
              std::string::npos);
    std::array<uint8_t, 160> After{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Buffer, After)), "");
    EXPECT_EQ(After, Before);
    std::array<uint8_t, 8> AfterCursor{};
    ASSERT_EQ(llvm::toString(Space.snapshotBacking(Saved, AfterCursor)), "");
    EXPECT_EQ(AfterCursor, Cursor);
  }
}
} // namespace
} // namespace neverd::emulation
