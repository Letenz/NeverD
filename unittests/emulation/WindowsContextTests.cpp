//===- WindowsContextTests.cpp - Caller context and native oracle -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcess.h"
#include "os/windows/process/WindowsProcessContext.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
#define NEVERD_CAPTURE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CAPTURE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsContextCases.def"
#undef NEVERD_CAPTURE_TEXT
#undef NEVERD_CAPTURE_VALUE
struct Case {
  const char *Name, *Argument;
};
constexpr Case Cases[] = {
#define NEVERD_CAPTURE_CASE(Name, Argument) {#Name, Argument},
#include "fixtures/WindowsContextCases.def"
#undef NEVERD_CAPTURE_CASE
};
std::string expected(const Case &C) {
  std::array<uint8_t, sizeof(uint32_t)> Bytes;
  llvm::support::endian::write32le(Bytes.data(), uint8_t(C.Argument[1]));
  return llvm::toHex(Bytes);
}
struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
  const char *Directory;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA, ISA##Dir},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class WindowsContext : public testing::TestWithParam<Profile> {
protected:
  ExecutionConfiguration Config;
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_CONTEXT_FIXTURE_DIR
    if (requireHvf(GetParam().Backend, GetParam().ISA))
      FAIL() << MissingTools;
    GTEST_SKIP() << MissingTools;
#else
    const auto &P = GetParam();
    Config.Backend = P.Backend;
    Config.Architecture = P.ISA;
    Config.Contract = P.ISA == GuestArchitecture::X64
                          ? ExecutionContract::CheckedUserX64
                          : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Config);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(P.Backend, P.ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Path = std::filesystem::path(NEVERD_WINDOWS_CONTEXT_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
};
TEST_P(WindowsContext, CapturesTheCallerAcrossProvidersAndCallbacks) {
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic << R->PC;
    EXPECT_EQ(R->ExitStatus, CompletionStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty()) << llvm::toHex(R->StandardError);
    EXPECT_EQ(llvm::toHex(R->StandardOutput), expected(C));
  }
}
// Test the same codec over real guest memory, including partial mappings.
// Predictable access failures must leave both the CPU and destination intact.
struct CaptureState {
  std::shared_ptr<AddressSpace> Space;
  BackendSelection Backend;
  std::vector<uint8_t> Initial;
  explicit CaptureState(const ExecutionConfiguration &Config)
      : Initial(2 * PageSize, Sentinel) {
    auto RAM = llvm::cantFail(PhysicalMemory::create(DirectLimit));
    Space = llvm::cantFail(AddressSpace::create(RAM, DirectLimit));
    llvm::cantFail(
        Space->map(DirectData, 2 * PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        Space->map(DirectCode, PageSize, Read | Execute | UserAccessible));
    llvm::cantFail(
        Space->map(DirectStack, PageSize, Read | Write | UserAccessible));
    Backend = llvm::cantFail(createExecutionBackend(Config, Space));
    auto &CPU = *Backend.CPU;
    const bool X64 = Config.Architecture == GuestArchitecture::X64;
    auto ABI = llvm::cantFail(
        IntegerABI::get(X64 ? IntegerCallingConvention::Win64
                            : IntegerCallingConvention::AAPCS64));
    const uint64_t SP =
        DirectStack + TestStackOffset - ABI.info().returnAddressSize();
    llvm::cantFail(CPU.writeRegister(ABI.info().StackPointer, {SP, 0}));
    llvm::cantFail(CPU.writeRegister(
        X64 ? CPURegister::X64PC : CPURegister::AArch64PC, {DirectCode, 0}));
    llvm::cantFail(CPU.writeRegister(
        X64 ? CPURegister::X64AX : CPURegister::AArch64X0, {LastErrorSeed, 0}));
    if (X64)
      llvm::cantFail(CPU.writeInteger(SP, TestReturnPC, sizeof(uint64_t)));
    else
      llvm::cantFail(CPU.writeRegister(ABI.info().Link, {TestReturnPC, 0}));
    llvm::cantFail(CPU.write(DirectData, Initial));
  }
  std::vector<uint8_t> bytes() {
    std::vector<uint8_t> Result(Initial.size());
    llvm::cantFail(Backend.CPU->read(DirectData, Result));
    return Result;
  }
};
TEST_P(WindowsContext, RejectsInvalidBuffersBeforePublishingState) {
  CaptureState State(Config);
  auto &CPU = *State.Backend.CPU;
  const auto Before = llvm::cantFail(windows_process::captureUserContext(CPU));
  const uint64_t Invalid[] = {0, DirectData + 1, DirectCode,
                              DirectData + 2 * PageSize - VectorBytes,
                              UINT64_MAX - VectorBytes + 1};
  for (uint64_t Address : Invalid) {
    SCOPED_TRACE(Address);
    auto E = windows_process::captureCallerContext(CPU, Address);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E))
                  .find(windows_process::text::CaptureContextBuffer),
              std::string::npos);
    EXPECT_EQ(State.bytes(), State.Initial);
    EXPECT_EQ(llvm::cantFail(windows_process::captureUserContext(CPU)), Before);
  }
  if (Config.Architecture == GuestArchitecture::X64) {
    llvm::cantFail(CPU.writeRegister(
        CPURegister::X64SP, {DirectStack + PageSize + sizeof(uint64_t), 0}));
    const auto BadStack =
        llvm::cantFail(windows_process::captureUserContext(CPU));
    auto E = windows_process::captureCallerContext(CPU, DirectData);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E))
                  .find(windows_process::text::CaptureContextReturn),
              std::string::npos);
    EXPECT_EQ(State.bytes(), State.Initial);
    EXPECT_EQ(llvm::cantFail(windows_process::captureUserContext(CPU)),
              BadStack);
  }
}
TEST_P(WindowsContext, CapturesAcrossPagesWithoutChangingTheCPU) {
  CaptureState State(Config);
  auto &CPU = *State.Backend.CPU;
  const bool X64 = Config.Architecture == GuestArchitecture::X64;
  const auto Before = llvm::cantFail(windows_process::captureUserContext(CPU));
  ASSERT_FALSE(bool(windows_process::captureCallerContext(
      CPU, DirectData + CrossPageOffset)));
  EXPECT_EQ(llvm::cantFail(windows_process::captureUserContext(CPU)), Before);
  const auto Bytes = State.bytes();
  const auto *Record = Bytes.data() + CrossPageOffset;
  EXPECT_EQ(llvm::support::endian::read64le(Record + (X64 ? X64SP : ARM64SP)),
            DirectStack + TestStackOffset);
  EXPECT_EQ(llvm::support::endian::read64le(Record + (X64 ? X64PC : ARM64PC)),
            TestReturnPC);
  EXPECT_EQ(
      llvm::support::endian::read64le(Record + (X64 ? X64GPR : ARM64X0Offset)),
      X64 ? LastErrorSeed : 0);
  if (!X64)
    EXPECT_EQ(llvm::support::endian::read64le(Record + ARM64LR), 0u);
  for (size_t I = 0; I < Bytes.size(); ++I) {
    const bool Captured =
        I >= CrossPageOffset &&
        (X64 ? ((I - CrossPageOffset >= X64FlagsOffset &&
                 I - CrossPageOffset < X64StatusOffset + sizeof(uint32_t)) ||
                (I - CrossPageOffset >= X64GPR &&
                 I - CrossPageOffset < X64TailOffset))
             : I - CrossPageOffset < ARM64TailOffset);
    if (!Captured)
      ASSERT_EQ(Bytes[I], Sentinel) << I;
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsContext, testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsContextNative, RunsOriginalCallerContextExecutable) {
#if !defined(_WIN32) || (!defined(_M_X64) && !defined(_M_ARM64))
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_CONTEXT_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
#if defined(_M_ARM64)
  constexpr auto Directory = AArch64Dir;
#else
  constexpr auto Directory = X64Dir;
#endif
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_CONTEXT_FIXTURE_DIR) / Directory /
       ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string(),
             Error = (Root / StderrFile).string();
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                      Error};
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    std::string Diagnostic;
    bool Failed = false;
    int Status = llvm::sys::ExecuteAndWait(
        Program, {Program, C.Argument}, std::nullopt, Redirects,
        NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
    ASSERT_FALSE(Failed) << Diagnostic;
    auto Out = llvm::MemoryBuffer::getFile(Output),
         Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << ObservationLabel << C.Argument << ' ' << Status << ' '
                 << llvm::toHex((*Out)->getBuffer()) << '\n';
    EXPECT_EQ(uint32_t(Status), CompletionStatus)
        << llvm::toHex((*Err)->getBuffer());
    EXPECT_TRUE((*Err)->getBuffer().empty())
        << llvm::toHex((*Err)->getBuffer());
    EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), expected(C));
  }
#endif
}
} // namespace
} // namespace neverd::emulation
