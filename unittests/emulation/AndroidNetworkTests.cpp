//===- AndroidNetworkTests.cpp - Offline resolver and TCP callers --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded offline network contracts and their semantic checks.

#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <cstring>
#include <tuple>
namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
const std::vector<uint8_t> IPv4{2, 0, 0x23, 0x29, 127, 0, 0, 1,
                                0, 0, 0,    0,    0,   0, 0, 0};
const std::vector<uint8_t> IPv6{10, 0, 0x23, 0x29, 0, 0, 0, 0, 0, 0,
                                0,  0, 0,    0,    0, 0, 0, 0, 0, 0,
                                0,  0, 0,    1,    0, 0, 0, 0};
class AndroidNetwork : public testing::TestWithParam<
                           std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross compiler unavailable";
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
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 4096});
    AndroidResolverQuery Q;
    Q.Node = "node.invalid";
    Q.Service = "9001";
    Q.Flags = 2;
    Q.SocketType = 1;
    Q.Addresses = {{1, 6, IPv4, "canonical.invalid"}, {1, 6, IPv6, {}}};
    Options.Android->ResolverQueries = std::vector{Q};
    Options.LinuxFiles.emplace();
    LinuxStreamConnection Stream;
    Stream.Peer = IPv4;
    Stream.ExpectedSend = {0, 0xff, 0x42, 0x13, 0x7f};
    Stream.Receive = {0x31, 0, 0xfe, 0x7f};
    Stream.ReceiveEOF = true;
    Stream.MaxSend = Stream.MaxReceive = 2;
    Options.LinuxNetwork.emplace().Connections.push_back(Stream);
    Options.Limits.Instructions = 100000;
#endif
  }
  ProcessResult run(const char *Entry, uint64_t Mode = 0) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = {Buffer, Mode};
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void returned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
  uint64_t word(const ProcessResult &R, unsigned I) {
    return llvm::support::endian::read64le(
        R.MemorySnapshots.at(0).Bytes.data() + 8 * I);
  }
};
TEST_P(AndroidNetwork,
       AddressParsingUsesAPI28DecimalAndRejectsMalformedInputs) {
  struct Case {
    unsigned Family;
    const char *Text;
    const char *Hex;
  };
  for (auto C : {Case{2, "127.0.0.1", "7f000001"},
                 {2, "001.002.003.004", "01020304"},
                 {2, "256.0.0.1", ""},
                 {2, "127.1", ""},
                 {2, "127.0.0.1 ", ""},
                 {2, "1.2.3.4.5", ""},
                 {10, "::", "00000000000000000000000000000000"},
                 {10, "::1", "00000000000000000000000000000001"},
                 {10, "2001:DB8::1", "20010db8000000000000000000000001"},
                 {10, "::ffff:127.0.0.1", "00000000000000000000ffff7f000001"},
                 {10, "1:2:3:4:5:6:7:8", "00010002000300040005000600070008"},
                 {10, "1:2:3:4:5:6:7::8", ""},
                 {10, "1::2::3", ""},
                 {10, ":::1", ""},
                 {10, "::1%2", ""},
                 {10, "12345::1", ""},
                 {10, "127.0.0.1::", ""}}) {
    std::copy(C.Text, C.Text + std::strlen(C.Text) + 1,
              Options.Android->Memory[0].Bytes.begin() + 512);
    auto R = run("network_pton", C.Family);
    returned(R);
    const auto HexBytes = llvm::fromHex(C.Hex);
    const std::vector<uint8_t> Expected(HexBytes.begin(), HexBytes.end());
    EXPECT_EQ(word(R, 0), Expected.empty() ? 0u : 1u) << C.Text;
    EXPECT_EQ(word(R, 1), 73u);
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    EXPECT_TRUE(
        std::equal(Expected.begin(), Expected.end(), Bytes.begin() + 64));
    EXPECT_EQ(Bytes[64 + Expected.size()], 0xa5);
  }
  auto R = run("network_pton", 999);
  returned(R);
  EXPECT_EQ(word(R, 0), UINT64_MAX);
  EXPECT_EQ(word(R, 1), 97u);
}

TEST_P(AndroidNetwork, AddressFormattingPreservesABIAndBufferBounds) {
  struct Case {
    unsigned Family;
    const char *Hex;
    const char *Text;
  };
  for (auto C : {Case{2, "7f000001", "127.0.0.1"},
                 {10, "00000000000000000000000000000000", "::"},
                 {10, "00000000000000000000000000000001", "::1"},
                 {10, "00000000000000000000ffff7f000001", "::ffff:127.0.0.1"},
                 {10, "000000000000000000000000c0000201", "::192.0.2.1"},
                 {10, "20010000000000010000000000010001", "2001::1:0:0:1:1"},
                 {10, "20010000000100010001000100010001", "2001:0:1:1:1:1:1:1"},
                 {10, "20010db8000000000000000000000000", "2001:db8::"}}) {
    auto Bytes = llvm::fromHex(C.Hex);
    std::copy(Bytes.begin(), Bytes.end(),
              Options.Android->Memory[0].Bytes.begin() + 512);
    const auto Length = std::strlen(C.Text);
    for (auto Size : {Length, Length + 1, size_t(64)}) {
      auto R = run("network_ntop", (uint64_t(Size) << 32) | C.Family);
      returned(R);
      const bool Fits = Size > Length;
      EXPECT_EQ(word(R, 0), Fits);
      EXPECT_EQ(word(R, 1), Fits ? 73u : 28u);
      const auto &Memory = R.MemorySnapshots[0].Bytes;
      if (Fits)
        EXPECT_TRUE(
            std::equal(C.Text, C.Text + Length + 1, Memory.begin() + 64));
      else
        EXPECT_EQ(Memory[64], 0xa5);
      EXPECT_EQ(Memory[64 + Length + 1], 0xa5);
    }
  }
  auto R = run("network_ntop", (uint64_t(64) << 32) | 999);
  returned(R);
  EXPECT_EQ(word(R, 0), 0u);
  EXPECT_EQ(word(R, 1), 97u);
}

TEST_P(AndroidNetwork, TimeoutOptionsUseSharedKernelAndErrno) {
  for (uint64_t Raw : {0, 1}) {
    auto R = run("network_timeout", Raw);
    returned(R);
    EXPECT_EQ(word(R, 0), 0u);
    EXPECT_EQ(word(R, 1), 0u);
    EXPECT_EQ(word(R, 2), 16u);
    EXPECT_EQ(word(R, 3), 73u);
    EXPECT_EQ(word(R, 4), 20u);
    EXPECT_EQ(word(R, 5), 0u);
    EXPECT_EQ(word(R, 6), 0xa5a5a5a5a5a5a5a5ULL);
  }
}

TEST_P(AndroidNetwork, ResolverPreservesOrderLayoutNamesAndErrno) {
  auto R = run("network_resolve");
  returned(R);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(word(R, 0), 0u);
  EXPECT_EQ(word(R, 1), 73u);
  EXPECT_EQ(word(R, 3), 2u);
  EXPECT_EQ(word(R, 68), 73u);
  const auto &Bytes = R.MemorySnapshots[0].Bytes;
  for (unsigned I = 0; I != 2; ++I) {
    const unsigned Start = 4 + I * 16;
    EXPECT_EQ(word(R, Start), 2u);
    EXPECT_EQ(word(R, Start + 1), I ? 10u : 2u);
    EXPECT_EQ(word(R, Start + 2), 1u);
    EXPECT_EQ(word(R, Start + 3), 6u);
    const auto &Address = I ? IPv6 : IPv4;
    EXPECT_EQ(word(R, Start + 4), Address.size());
    EXPECT_TRUE(std::equal(Address.begin(), Address.end(),
                           Bytes.begin() + 8 * (Start + 5)));
    EXPECT_EQ(Bytes[8 * (Start + 5) + Address.size()], 0xa5);
  }
  const char Name[] = "canonical.invalid";
  EXPECT_TRUE(
      std::equal(std::begin(Name), std::end(Name), Bytes.begin() + 13 * 8));
  EXPECT_EQ(Bytes[13 * 8 + sizeof(Name)], 0xa5);
  EXPECT_EQ(word(R, 29), 0xa5a5a5a5a5a5a5a5ULL);
}
TEST_P(AndroidNetwork, ResolverErrorsPreserveNullOrderingAndSystemErrno) {
  for (auto [Mode, Error] :
       {std::pair(1u, 3u), {2u, 5u}, {3u, 12u}, {4u, 8u}}) {
    auto R = run("network_resolve", Mode);
    returned(R);
    EXPECT_EQ(word(R, 0), Error);
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 2), Mode == 4 ? 0x1234u : 0u);
  }
  auto &Q = Options.Android->ResolverQueries->front();
  Q.Addresses.clear();
  for (unsigned Error : {2u, 8u, 11u}) {
    Q.Error = Error;
    Q.SystemErrno = Error == 11 ? 101 : 0;
    auto R = run("network_resolve");
    returned(R);
    EXPECT_EQ(word(R, 0), Error);
    EXPECT_EQ(word(R, 1), Error == 11 ? 101u : 73u);
    EXPECT_EQ(word(R, 2), 0u);
  }
}
TEST_P(AndroidNetwork, MissingQueryAndRepeatedFreeNeverInventResults) {
  auto R = run("network_resolve", 5);
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(R.Diagnostic.find("resolver-owned"), std::string::npos);
  Options.Android->ResolverQueries->front().Service = "9002";
  R = run("network_resolve");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("exact offline observation"), std::string::npos);
}
TEST_P(AndroidNetwork, StreamAliasesShortTransfersAndRawWrappersShareState) {
  for (unsigned Raw : {0, 1}) {
    auto R = run("network_stream", Raw);
    returned(R);
    const uint64_t Expected[] = {2, 2, 1, 2, 2, 0, 0, UINT64_MAX, 9};
    for (unsigned I = 0; I < std::size(Expected); ++I)
      EXPECT_EQ(word(R, I + 2), Expected[I]) << I;
    const std::vector<uint8_t> Guarded{0xa5, 0x31, 0,    0xfe,
                                       0x7f, 0xa5, 0xa5, 0xa5};
    const auto &Bytes = R.MemorySnapshots[0].Bytes;
    EXPECT_TRUE(
        std::equal(Guarded.begin(), Guarded.end(), Bytes.begin() + 128));
  }
}
TEST_P(AndroidNetwork, RefusalAndUnknownPeerStaySeparate) {
  auto &C = Options.LinuxNetwork->Connections.front();
  C.ConnectErrno = 111;
  C.ExpectedSend.clear();
  C.Receive.clear();
  C.ReceiveEOF = false;
  auto R = run("network_stream");
  returned(R);
  EXPECT_EQ(word(R, 0), UINT32_MAX);
  EXPECT_EQ(word(R, 1), 111u);
  C.Peer[3]++;
  R = run("network_stream");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("matching offline connection"),
            std::string::npos);
}
TEST_P(AndroidNetwork, DynamicResolverUsesTheSameOwner) {
  Options.Android->Libraries["liboffline.so"] = {"getaddrinfo", "freeaddrinfo"};
  auto R = run("network_dynamic");
  returned(R);
  EXPECT_EQ(word(R, 0), 0u);
  EXPECT_EQ(word(R, 1), 2u);
}
INSTANTIATE_TEST_SUITE_P(
    Native, AndroidNetwork,
    testing::Combine(testing::Values("network-O0-none", "network-O0-android",
                                     "network-O0-relr", "network-O2-none",
                                     "network-O2-android", "network-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
