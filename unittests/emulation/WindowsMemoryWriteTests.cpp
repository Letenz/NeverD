//===- WindowsMemoryWriteTests.cpp - Original cross-page API behavior -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <filesystem>
#include <iterator>

namespace neverd::emulation {
namespace {
#define NEVERD_MEMORY_WRITE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MEMORY_WRITE_WIDE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_MEMORY_WRITE_PROTECTION(Name, Value)                            \
  constexpr uint64_t Name = Value;
#define NEVERD_MEMORY_WRITE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_TEXT
#undef NEVERD_MEMORY_WRITE_PROTECTION
#undef NEVERD_MEMORY_WRITE_WIDE
#undef NEVERD_MEMORY_WRITE_VALUE

enum ProtectionIndex {
#define NEVERD_MEMORY_WRITE_PROTECTION(Name, Value) Name##Index,
#include "fixtures/WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_PROTECTION
  ProtectionCount
};
struct Observation {
#define NEVERD_MEMORY_WRITE_FIELD(Name) uint64_t Name;
#include "fixtures/WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_FIELD
};
constexpr Observation Expected[] = {
#define NEVERD_MEMORY_WRITE_EXPECTED(First, Second, Result, Error, Written,    \
                                     AfterFirst, AfterSecond, ByteFirst,       \
                                     ByteSecond)                               \
  {First##Index, Second##Index, Result,    Error,     Written,                 \
   AfterFirst,   AfterSecond,   ByteFirst, ByteSecond},
#include "fixtures/WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_EXPECTED
};
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  ExecutionContract Contract;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA,               \
   ExecutionContract::Contract, #ISA},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsMemoryWrite : public testing::TestWithParam<Profile> {};

TEST_P(WindowsMemoryWrite, MatchesNativeProtectionAndPartialWriteMatrix) {
#ifndef NEVERD_WINDOWS_MEMORY_WRITE_FIXTURE_DIR
  if (requireHvf(GetParam().Backend, GetParam().ISA))
    FAIL() << MissingTools;
  GTEST_SKIP() << MissingTools;
#else
  const auto &P = GetParam();
  ExecutionConfiguration Configuration;
  Configuration.Backend = P.Backend;
  Configuration.Architecture = P.ISA;
  Configuration.Contract = P.Contract;
  auto Probe = probeExecutionBackend(Configuration);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available) {
    if (requireHvf(P.Backend, P.ISA))
      FAIL() << Probe->Reason;
    GTEST_SKIP() << Probe->Reason;
  }
  const auto Path =
      std::filesystem::path(NEVERD_WINDOWS_MEMORY_WRITE_FIXTURE_DIR) /
      P.Directory / ProgramFile;
  ProcessOptions Options;
  Options.Backend = P.Backend;
  Options.Limits.Instructions = InstructionLimit;
  Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
  auto Result = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_EQ(Result->Stop, ProcessStopReason::Exited) << Result->Diagnostic;
  ASSERT_EQ(Result->ExitStatus, CompletionStatus)
      << llvm::toHex(Result->StandardError);
  ASSERT_TRUE(Result->StandardError.empty());
  ASSERT_EQ(std::size(Expected), ProtectionCount * ProtectionCount);
  ASSERT_EQ(Result->StandardOutput.size(), std::size(Expected) * RecordSize);
  const auto *Bytes =
      reinterpret_cast<const uint8_t *>(Result->StandardOutput.data());
  for (const auto &E : Expected) {
    SCOPED_TRACE(testing::Message() << E.First << "," << E.Second);
#define NEVERD_MEMORY_WRITE_FIELD(Name)                                        \
  EXPECT_EQ(llvm::support::endian::read64le(Bytes), E.Name) << #Name;          \
  Bytes += sizeof(uint64_t);
#include "fixtures/WindowsMemoryWriteCases.def"
#undef NEVERD_MEMORY_WRITE_FIELD
  }
#endif
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsMemoryWrite,
                         testing::ValuesIn(Profiles),
                         [](const testing::TestParamInfo<Profile> &Info) {
                           return Info.param.Name;
                         });
} // namespace
} // namespace neverd::emulation
