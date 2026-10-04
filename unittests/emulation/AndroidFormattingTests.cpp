//===- AndroidFormattingTests.cpp - Compiled AAPCS64 formatting calls
//------===//
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
class AndroidFormatting : public testing::TestWithParam<const char *> {
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
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 1024});
    Options.InstructionQuantum = 7;
    Options.Limits.Instructions = 500000;
#endif
  }
  void put(uint64_t Offset, llvm::StringRef Text) {
    auto &Bytes = Options.Android->Memory[0].Bytes;
    std::copy(Text.bytes_begin(), Text.bytes_end(), Bytes.begin() + Offset);
    Bytes[Offset + Text.size()] = 0;
  }
  ProcessResult run(llvm::StringRef Entry, std::vector<uint64_t> Arguments) {
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
  }
  void output(const ProcessResult &R, llvm::StringRef Text, size_t Offset = 0) {
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    ASSERT_LT(Offset + Text.size() + 1, Bytes.size());
    EXPECT_EQ(std::vector<uint8_t>(Bytes.begin() + Offset,
                                   Bytes.begin() + Offset + Text.size()),
              std::vector<uint8_t>(Text.bytes_begin(), Text.bytes_end()));
    EXPECT_EQ(Bytes[Offset + Text.size()], 0);
    EXPECT_EQ(Bytes[Offset + Text.size() + 1], 0xa5);
  }
};

TEST_P(AndroidFormatting, FourEntrypointsPromotionsLongValuesAndVACopy) {
  const std::string Expected =
      "-0000123|0xfedcba9876543210|abc    |-128/255/-32768/65535|"
      "-9223372036854775808/18446744073709551615|4294967297/-4294967298/-5|"
      "\x80/%|0x123456789abcdef0";
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("format_integers", {Buffer, Mode});
    returned(R, Expected.size());
    output(R, Expected);
    if (Mode % 2)
      output(R, Expected, 256);
    unsigned Calls = 0;
    const char *Names[] = {"snprintf", "vsnprintf", "sprintf", "vsprintf"};
    for (const auto &Call : R.NativeCalls)
      if (Call.Name == Names[Mode]) {
        ++Calls;
        EXPECT_EQ(Call.Result, Expected.size());
      }
    EXPECT_EQ(Calls, Mode % 2 ? 2u : 1u);
  }
}
TEST_P(AndroidFormatting, ExhaustedGPRegisterAreaStartsOnStack) {
  const char Expected[] = "-17:4294967298:stack:002a    ";
  auto R = run("format_stacked", {Buffer});
  returned(R, sizeof(Expected) - 1);
  output(R, Expected);
}
TEST_P(AndroidFormatting, FlagsPrecisionNullPointersAndAndroidOctalBehavior) {
  const char Expected[] =
      "[00023   ][     002a][011][][][+][0x][0x0][(nu][whole]";
  auto R = run("format_flags", {Buffer});
  returned(R, sizeof(Expected) - 1);
  output(R, Expected);
}
TEST_P(AndroidFormatting, BinaryCharactersAndTruncationReportRequiredLength) {
  const std::vector<uint8_t> Full = {'A', 0, 'B', 0x80, 0xff, 'Z'};
  for (uint64_t Capacity : {0, 1, 2, 4, 6, 7, 64}) {
    auto R = run("format_binary", {Capacity ? Buffer : 0, Capacity});
    returned(R, Full.size());
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(1024, 0xa5);
    if (Capacity) {
      size_t Count = std::min(Capacity - 1, uint64_t(Full.size()));
      std::copy_n(Full.begin(), Count, Expected.begin());
      Expected[Count] = 0;
    }
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}
TEST_P(AndroidFormatting, HugePaddingCountsWithoutHugeAllocation) {
  put(512, "%2147483647d");
  auto R = run("format_supplied", {Buffer, 4, Buffer + 512, 1, 0, 0});
  returned(R, 2147483647);
  output(R, "   ");
}
TEST_P(AndroidFormatting, PrecisionNeverReadsBeyondTheLastMappedByte) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  std::copy_n("tail", 4, Bytes.begin() + 8188);
  auto R = run("format_precision_page", {Buffer, Buffer + 8188});
  returned(R, 4);
  output(R, "tail");
  put(512, "%.0s");
  R = run("format_one_string", {Buffer, 16, Buffer + 512, UINT64_MAX});
  returned(R, 0);
  output(R, "");
}
TEST_P(AndroidFormatting, CapacityDoesNotRequireUnusedWritableMemory) {
  Options.Android->ReadMemory = {{Buffer + 4094, 16}};
  auto R = run("format_readonly_tail", {Buffer + 4094});
  returned(R, 1);
  output(R, "x");
}
TEST_P(AndroidFormatting, UnsupportedConversionsAndOverflowPublishNoResult) {
  for (unsigned Mode = 0; Mode < 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("format_unsafe", {Buffer, Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_EQ(R.NativeCalls.back().Name, "snprintf");
    EXPECT_FALSE(R.NativeCalls.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(1024, 0xa5));
  }
}
TEST_P(AndroidFormatting, BadFormatStringAndDestinationFailExplicitly) {
  put(512, "%s");
  for (auto Args : {std::vector<uint64_t>{Buffer, 16, UINT64_MAX, 0},
                    {Buffer, 16, Buffer + 512, UINT64_MAX},
                    {Buffer + 8191, 16, Buffer + 512, Buffer + 512}}) {
    auto R = run("format_one_string", Args);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
  }
}
TEST_P(AndroidFormatting, DiscardedOutputStillRequiresTerminatedInput) {
  // An accessible but unterminated input must still fail for snprintf(NULL,0).
  put(512, "%s");
  auto R = run("format_one_string", {0, 0, Buffer + 512, Buffer + 1024});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
}
TEST_P(AndroidFormatting, ExplicitVAListUsesItsOwnSavedRegistersAndStack) {
  auto &Bytes = Options.Android->Memory[0].Bytes;
  // One remaining GP slot, then the separately named overflow stack.
  llvm::support::endian::write64le(Bytes.data() + 512, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 520, Buffer + 704);
  llvm::support::endian::write64le(Bytes.data() + 528, 1); // Unused FP area.
  llvm::support::endian::write32le(Bytes.data() + 536, -8);
  llvm::support::endian::write32le(Bytes.data() + 540, -128);
  llvm::support::endian::write64le(Bytes.data() + 696, 0xfedcba98fffffff9);
  llvm::support::endian::write64le(Bytes.data() + 704, 0x10000000a);
  put(576, "%d/%lx");
  auto R = run("format_explicit_va", {Buffer, Buffer + 512, Buffer + 576});
  returned(R, 12);
  output(R, "-7/10000000a");
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(std::vector<uint8_t>(R.MemorySnapshots[0].Bytes.begin() + 512,
                                 R.MemorySnapshots[0].Bytes.end()),
            std::vector<uint8_t>(Bytes.begin() + 512, Bytes.begin() + 1024));
  for (int Offset : {-72, -7}) {
    llvm::support::endian::write32le(Bytes.data() + 536, Offset);
    R = run("format_explicit_va", {Buffer, Buffer + 512, Buffer + 576});
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
  }
  llvm::support::endian::write32le(Bytes.data() + 536, 0);
  llvm::support::endian::write64le(Bytes.data() + 512, UINT64_MAX - 7);
  R = run("format_explicit_va", {Buffer, Buffer + 512, Buffer + 576});
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
}
TEST_P(AndroidFormatting, DynamicResolutionRetainsProviderAndClosedCallFails) {
  Options.Android->Libraries["libformat-model.so"] = {"snprintf"};
  auto R = run("format_dynamic", {Buffer, 0});
  returned(R, 18);
  output(R, "symbol=0x10000000a");
  std::optional<uint64_t> Address;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name == "dlsym")
      Address = Call.Result;
    if (Call.Name == "snprintf") {
      EXPECT_EQ(Call.Library, "libformat-model.so");
      EXPECT_EQ(Call.PC, Address);
      EXPECT_EQ(Call.Result, 18u);
    }
  }
  ASSERT_TRUE(Address);
  R = run("format_dynamic", {Buffer, 1});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_FALSE(R.NativeCalls.back().Result);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(1024, 0xa5));
}
INSTANTIATE_TEST_SUITE_P(OptimizationAndPacking, AndroidFormatting,
                         testing::Values("format-O0-none", "format-O0-android",
                                         "format-O0-relr", "format-O2-none",
                                         "format-O2-android",
                                         "format-O2-relr"));
} // namespace
} // namespace neverd::emulation
