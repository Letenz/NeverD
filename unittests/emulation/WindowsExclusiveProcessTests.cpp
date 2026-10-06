//===- WindowsExclusiveProcessTests.cpp - Native ARM64 access parity ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SHA256.h"

#include <array>

namespace neverd::emulation {
namespace {
#define NEVERD_EXCLUSIVE_ORACLE_VALUE(Name, Value)                             \
  constexpr uint64_t Name = Value;
#define NEVERD_EXCLUSIVE_ORACLE_WIDE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#define NEVERD_EXCLUSIVE_ORACLE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "fixtures/AArch64ExclusiveOracle.def"
#undef NEVERD_EXCLUSIVE_ORACLE_VALUE
#undef NEVERD_EXCLUSIVE_ORACLE_WIDE
#undef NEVERD_EXCLUSIVE_ORACLE_TEXT
#define NEVERD_EXCLUSIVE_NATIVE_VALUE(Name, Value)                             \
  constexpr uint64_t Name = Value;
#define NEVERD_EXCLUSIVE_NATIVE_TEXT(Name, Value) constexpr char Name[] = Value;
#include "fixtures/WindowsExclusiveNative.def"
#undef NEVERD_EXCLUSIVE_NATIVE_VALUE
#undef NEVERD_EXCLUSIVE_NATIVE_TEXT
enum Field {
#define NEVERD_EXCLUSIVE_ORACLE_FIELD(Name) Name,
#include "fixtures/AArch64ExclusiveOracle.def"
#undef NEVERD_EXCLUSIVE_ORACLE_FIELD
  FieldCount
};
struct NativeCase {
  const char *Name;
  unsigned Records;
  const char *Digest;
};
constexpr NativeCase Cases[] = {
#define NEVERD_EXCLUSIVE_NATIVE_CASE(Name, Records, Digest)                    \
  {#Name, Records, Digest},
#include "fixtures/WindowsExclusiveNative.def"
#undef NEVERD_EXCLUSIVE_NATIVE_CASE
};
struct Profile {
  ExecutionBackendKind Backend;
  const char *Name;
};
constexpr Profile Profiles[] = {
#define NEVERD_EXCLUSIVE_NATIVE_BACKEND(Name)                                  \
  {ExecutionBackendKind::Name, #Name},
#include "fixtures/WindowsExclusiveNative.def"
#undef NEVERD_EXCLUSIVE_NATIVE_BACKEND
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsExclusive : public testing::TestWithParam<Profile> {};

TEST_P(WindowsExclusive, MatchesNativeAlignmentPermissionsAndRegisterContexts) {
#ifndef NEVERD_WINDOWS_PROCESS_FIXTURE_DIR
  GTEST_SKIP() << MissingTools;
#else
  ExecutionConfiguration Config;
  Config.Architecture = GuestArchitecture::AArch64;
  Config.Backend = GetParam().Backend;
  Config.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(Config);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  ProcessOptions Options;
  Options.Backend = GetParam().Backend;
  Options.Limits = {InstructionLimit, EventLimit, TimeoutMicroseconds};
  Options.OutputLimit = OutputLimit;
  const auto Path =
      std::filesystem::path(NEVERD_WINDOWS_PROCESS_FIXTURE_DIR) / Program;
  auto Result = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  ASSERT_EQ(Result->ExitStatus, 0u) << llvm::toHex(Result->StandardError);
  ASSERT_TRUE(Result->StandardError.empty())
      << llvm::toHex(Result->StandardError);
  uint64_t ExpectedSize = std::size(Cases) * sizeof(uint32_t);
  for (const auto &C : Cases)
    ExpectedSize += C.Records * FieldCount * sizeof(uint64_t);
  llvm::StringRef Bytes(Result->StandardOutput);
  ASSERT_EQ(Bytes.size(), ExpectedSize);
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    ASSERT_EQ(llvm::support::endian::read32le(Bytes.data()), 1u);
    Bytes = Bytes.drop_front(sizeof(uint32_t));
  }
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    llvm::SHA256 Digest;
    for (unsigned N = 0; N < C.Records; ++N) {
      std::array<uint64_t, FieldCount> Row;
      for (auto &Value : Row) {
        Value = llvm::support::endian::read64le(Bytes.data());
        Bytes = Bytes.drop_front(sizeof(uint64_t));
      }
      SCOPED_TRACE(N);
      ASSERT_NE(Row[LoadPC], Row[StorePC]);
      ASSERT_NE(Row[LoadPC], 0u);
      ASSERT_NE(Row[StorePC], 0u);
      ASSERT_EQ(Row[LoadPC] % InstructionBytes, 0u);
      ASSERT_EQ(Row[StorePC] % InstructionBytes, 0u);
      ASSERT_NE(Row[MemoryBase], 0u);
      ASSERT_EQ(Row[MemoryBase] % WindowAlignment, 0u);
      const uint64_t PC =
          Row[Faults] ? Row[FaultStage] ? Row[StorePC] : Row[LoadPC] : 0;
      for (auto F : {FaultPC, ExceptionPC, ContextPC})
        EXPECT_EQ(Row[F], PC);
      if (Row[Faults]) {
        ASSERT_GE(Row[ContextAddress], Row[MemoryBase]);
        Row[ContextAddress] -= Row[MemoryBase];
      }
      if (Row[ParameterCount] == 2) {
        ASSERT_EQ(Row[Code], AccessViolation);
        ASSERT_GE(Row[Parameter1], Row[MemoryBase]);
        Row[Parameter1] -= Row[MemoryBase];
      }
      for (auto F :
           {LoadPC, StorePC, FaultPC, ExceptionPC, ContextPC, MemoryBase})
        Row[F] = 0;
      std::array<uint8_t, FieldCount * sizeof(uint64_t)> Normalized;
      for (unsigned I = 0; I < FieldCount; ++I)
        llvm::support::endian::write64le(
            Normalized.data() + I * sizeof(uint64_t), Row[I]);
      Digest.update(Normalized);
    }
    EXPECT_EQ(llvm::toHex(Digest.final(), true), C.Digest);
  }
  EXPECT_TRUE(Bytes.empty());
#endif
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsExclusive,
                         testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });
} // namespace
} // namespace neverd::emulation
