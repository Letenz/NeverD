//===- WindowsExceptionTests.cpp - User exception and native oracles -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/process/WindowsProcessContext.h"
#include "os/windows/process/WindowsProcessExceptions.h"

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

#if defined(_WIN32)
#include <windows.h>
#endif

namespace neverd::emulation {
namespace {
namespace win = windows_process;
#define NEVERD_VEH_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_VEH_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsExceptionCases.def"
#undef NEVERD_VEH_TEXT
#undef NEVERD_VEH_VALUE
#if defined(EXCEPTION_SOFTWARE_ORIGINATE)
static_assert(SoftwareOriginate == EXCEPTION_SOFTWARE_ORIGINATE);
#endif
namespace x64_context {
#define NEVERD_VEH_CONTEXT_X64(Name, Value) constexpr uint64_t Name = Value;
#include "fixtures/WindowsExceptionCases.def"
#undef NEVERD_VEH_CONTEXT_X64
} // namespace x64_context
namespace arm_context {
#define NEVERD_VEH_CONTEXT_ARM64(Name, Value) constexpr uint64_t Name = Value;
#include "fixtures/WindowsExceptionCases.def"
#undef NEVERD_VEH_CONTEXT_ARM64
} // namespace arm_context
#if defined(_WIN32) && defined(_M_X64)
static_assert(sizeof(CONTEXT) == x64_context::ContextSize);
static_assert(offsetof(CONTEXT, Rip) == x64_context::ContextPC);
static_assert(offsetof(CONTEXT, EFlags) == x64_context::ContextProcessorFlags);
static_assert(offsetof(CONTEXT, Rsp) == x64_context::ContextSP);
static_assert(offsetof(CONTEXT, Rax) == x64_context::ContextResult);
static_assert(offsetof(CONTEXT, Rcx) == x64_context::ContextPointer);
static_assert(offsetof(CONTEXT, Xmm0) == x64_context::ContextVector);
static_assert(offsetof(CONTEXT, MxCsr) == x64_context::ContextControl);
static_assert(offsetof(CONTEXT, FltSave.MxCsr) ==
              x64_context::ContextControlCopy);
#elif defined(_WIN32) && defined(_M_ARM64)
static_assert(sizeof(CONTEXT) == arm_context::ContextSize);
static_assert(offsetof(CONTEXT, Pc) == arm_context::ContextPC);
static_assert(offsetof(CONTEXT, Sp) == arm_context::ContextSP);
static_assert(offsetof(CONTEXT, X0) == arm_context::ContextResult);
static_assert(offsetof(CONTEXT, V) == arm_context::ContextVector);
static_assert(offsetof(CONTEXT, Fpcr) == arm_context::ContextControl);
#endif
struct Case {
  const char *Name, *Argument, *Expected;
  bool X64Only;
  ExecutionFeature Required = ExecutionFeature::None;
};
constexpr Case Cases[] = {
#define NEVERD_VEH_CASE(Name, Argument, Expected, X64Only)                     \
  {#Name, Argument, Expected, X64Only},
#define NEVERD_VEH_SIMD_CASE(Name, Argument, Expected)                         \
  {#Name, Argument, Expected, true, ExecutionFeature::SIMDExceptions},
#include "fixtures/WindowsExceptionCases.def"
#undef NEVERD_VEH_SIMD_CASE
#undef NEVERD_VEH_CASE
};
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
class WindowsExceptions : public testing::TestWithParam<Profile> {
protected:
  ExecutionConfiguration Config;
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR
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
    Path = std::filesystem::path(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR) /
           P.Directory / ProgramFile;
    Options.Backend = P.Backend;
    Options.Windows.emplace();
    Options.Windows->Modules.push_back(
        {LibraryFile, Path.parent_path() / LibraryFile});
    Options.Limits.Instructions = InstructionLimit;
    Options.Limits.TimeoutMicroseconds = TimeoutMicroseconds;
#endif
  }
};
TEST_P(WindowsExceptions, ExecutesOriginalExceptionScenarios) {
  const auto Caps = llvm::cantFail(executionCapabilities(
      Config.Contract, Config.Architecture, Config.Backend));
  for (const auto &C : Cases) {
    if ((C.X64Only && GetParam().ISA != GuestArchitecture::X64) ||
        !Caps.supports(C.Required))
      continue;
    SCOPED_TRACE(C.Name);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Exited) << R->Diagnostic;
    EXPECT_EQ(R->ExitStatus, ExitStatus) << llvm::toHex(R->StandardError);
    EXPECT_TRUE(R->StandardError.empty()) << llvm::toHex(R->StandardError);
    EXPECT_EQ(llvm::toHex(R->StandardOutput), C.Expected);
  }
}
TEST_P(WindowsExceptions, RejectsUnsupportedDispatchAndInvalidContinuations) {
  struct Negative {
    const char *Argument, *Diagnostic;
    ExecutionFeature NativeContinuation = ExecutionFeature::None;
  };
  const Negative Cases[] = {
#define NEVERD_VEH_NEGATIVE(Argument, Diagnostic)                              \
  {Argument, win::text::Diagnostic},
#define NEVERD_VEH_SIMD_CASE(Name, Argument, Expected)                         \
  {Argument, win::text::ExceptionContext, ExecutionFeature::SIMDExceptions},
#include "fixtures/WindowsExceptionCases.def"
#undef NEVERD_VEH_SIMD_CASE
#undef NEVERD_VEH_NEGATIVE
  };
  const auto Caps = llvm::cantFail(executionCapabilities(
      Config.Contract, Config.Architecture, Config.Backend));
  for (const auto &C : Cases) {
    // A native success is asserted in ExecutesOriginalExceptionScenarios and
    // in the original Windows executable oracle, never silently omitted.
    if (C.NativeContinuation != ExecutionFeature::None &&
        Caps.supports(C.NativeContinuation))
      continue;
    SCOPED_TRACE(C.Argument);
    Options.Arguments = {ProgramFile, C.Argument};
    auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::RuntimeFailure) << R->Diagnostic;
    EXPECT_NE(R->Diagnostic.find(C.Diagnostic), std::string::npos)
        << R->Diagnostic;
    EXPECT_FALSE(R->ExitStatus);
    EXPECT_TRUE(R->StandardOutput.empty());
    EXPECT_TRUE(R->StandardError.empty()) << llvm::toHex(R->StandardError);
  }
}
TEST_P(WindowsExceptions, NestedHandlersShareTheProcessEventBudget) {
  Options.Arguments = {ProgramFile, RecursiveArgument};
  Options.Limits.Events = SmallEventLimit;
  auto R = emulateProcess(Path, ProcessProfile::WindowsPE64, Options);
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->Stop, ProcessStopReason::EventLimit) << R->Diagnostic;
  EXPECT_EQ(R->Events, SmallEventLimit);
  EXPECT_FALSE(R->ExitStatus);
  EXPECT_TRUE(R->StandardOutput.empty());
  EXPECT_TRUE(R->StandardError.empty());
}
TEST_P(WindowsExceptions, ContextValidationPreservesStoppedCPUAndMemory) {
  const bool X64 = GetParam().ISA == GuestArchitecture::X64;
  auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  const uint64_t StackBase = win::value::StackTop - Options.StackSize;
  llvm::cantFail(Space->map(win::value::GateBase, PageSize,
                            Read | Execute | UserAccessible));
  llvm::cantFail(
      Space->map(StackBase, Options.StackSize, Read | Write | UserAccessible));
  auto Backend = llvm::cantFail(createExecutionBackend(Config, Space));
  auto &CPU = *Backend.CPU;
  const auto PC = X64 ? CPURegister::X64PC : CPURegister::AArch64PC;
  const auto SP = X64 ? CPURegister::X64SP : CPURegister::AArch64SP;
  const auto R = X64 ? CPURegister::X64AX : CPURegister::AArch64X0;
  llvm::cantFail(CPU.writeRegister(PC, {win::value::GateBase, 0}));
  llvm::cantFail(CPU.writeRegister(SP, {win::value::StackTop - PageSize, 0}));
  llvm::cantFail(CPU.writeRegister(R, {DataValue, 0}));
  const auto VectorCount =
      X64 ? x64_context::ContextVectorCount : arm_context::ContextVectorCount;
  for (unsigned I = 0; I < VectorCount; ++I)
    llvm::cantFail(
        CPU.writeRegister(vectorRegister(GetParam().ISA, I),
                          {VectorInitialLow + I, VectorInitialHigh - I}));
  uint64_t Mask = 0;
  if (X64) {
    Mask = llvm::cantFail(CPU.supportedControlBits(CPURegister::X64MXCSR))[0];
    llvm::cantFail(
        CPU.setReg(X64Register::MXCSR, x64_context::DAZControl & Mask));
  }
  auto Original = llvm::cantFail(win::captureUserContext(CPU));
  if (X64)
    EXPECT_EQ(llvm::support::endian::read32le(Original.data() +
                                              x64_context::ContextControlMask),
              Mask);
  auto Snapshot = llvm::cantFail(CPU.saveContext());
  llvm::cantFail(CPU.writeRegister(R, {VectorLow, 0}));
  llvm::cantFail(CPU.writeInteger(StackBase, VectorHigh, sizeof(uint64_t)));
  const auto Before = llvm::cantFail(win::captureUserContext(CPU));
  std::vector<uint64_t> InvalidOffsets = {
      X64 ? x64_context::ContextFlags : arm_context::ContextFlags,
      X64 ? x64_context::ContextPC : arm_context::ContextPC,
      X64 ? x64_context::ContextSP : arm_context::ContextSP};
  if (X64)
    InvalidOffsets.push_back(x64_context::ContextControlMask);
  for (uint64_t Offset : InvalidOffsets) {
    auto Changed = Original;
    llvm::support::endian::write64le(Changed.data() + Offset, 0);
    auto E = win::restoreUserContext(CPU, *Snapshot, Original, Changed,
                                     StackBase, win::value::StackTop);
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(win::text::ExceptionContext),
              std::string::npos);
    EXPECT_EQ(llvm::cantFail(win::captureUserContext(CPU)), Before);
  }
  auto Changed = Original;
  llvm::support::endian::write64le(
      Changed.data() +
          (X64 ? x64_context::ContextResult : arm_context::ContextResult),
      VectorInitialLow);
  const uint64_t VectorOffset =
      (X64 ? x64_context::ContextVector : arm_context::ContextVector) +
      (VectorCount - 1) * sizeof(RegisterValue);
  llvm::support::endian::write64le(Changed.data() + VectorOffset, VectorLow);
  llvm::support::endian::write64le(
      Changed.data() + VectorOffset + sizeof(uint64_t), VectorHigh);
  llvm::cantFail(win::restoreUserContext(CPU, *Snapshot, Original, Changed,
                                         StackBase, win::value::StackTop));
  if (X64)
    EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::MXCSR)),
              x64_context::DAZControl & Mask);
  EXPECT_EQ(llvm::cantFail(CPU.readRegister(R))[0], VectorInitialLow);
  for (unsigned I = 0; I < VectorCount; ++I) {
    const RegisterValue Expected =
        I + 1 == VectorCount
            ? RegisterValue{VectorLow, VectorHigh}
            : RegisterValue{VectorInitialLow + I, VectorInitialHigh - I};
    EXPECT_EQ(
        llvm::cantFail(CPU.readRegister(vectorRegister(GetParam().ISA, I))),
        Expected);
  }
  EXPECT_EQ(llvm::cantFail(CPU.readInteger(StackBase, sizeof(uint64_t))),
            VectorHigh);
  EXPECT_FALSE(win::ExceptionDispatcher::recoverable(
      GetParam().ISA,
      {BackendFaultKind::InvalidInstruction, win::value::GateBase}));
}
TEST_P(WindowsExceptions, FaultContextPreservesLogicalFlagsAndRejectsRFEdits) {
  if (GetParam().ISA != GuestArchitecture::X64)
    GTEST_SKIP() << X64FaultContextOnly;
  auto RAM = llvm::cantFail(PhysicalMemory::create(Options.MemoryLimit));
  auto Space = llvm::cantFail(AddressSpace::create(RAM, Options.MemoryLimit));
  const uint64_t StackBase = win::value::StackTop - Options.StackSize;
  llvm::cantFail(Space->map(win::value::GateBase, PageSize,
                            Read | Execute | UserAccessible));
  llvm::cantFail(
      Space->map(StackBase, Options.StackSize, Read | Write | UserAccessible));
  auto Backend = llvm::cantFail(createExecutionBackend(Config, Space));
  auto &CPU = *Backend.CPU;
  llvm::cantFail(CPU.setReg(X64Register::PC, win::value::GateBase));
  llvm::cantFail(CPU.setReg(X64Register::SP, win::value::StackTop - PageSize));
  llvm::cantFail(CPU.setReg(X64Register::AX, DataValue));
  llvm::cantFail(CPU.writeInteger(StackBase, VectorHigh, sizeof(uint64_t)));
  const auto InitialFlags = llvm::cantFail(CPU.reg(X64Register::FLAGS));
  for (auto Origin :
       {win::ContextOrigin::Current, win::ContextOrigin::HardwareFault}) {
    const bool Fault = Origin == win::ContextOrigin::HardwareFault;
    auto Original = llvm::cantFail(win::captureUserContext(CPU, Origin));
    auto Snapshot = llvm::cantFail(CPU.saveContext());
    EXPECT_EQ(llvm::support::endian::read32le(
                  Original.data() + x64_context::ContextProcessorFlags),
              InitialFlags | (Fault ? FaultResumeFlag : 0));
    EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FLAGS)), InitialFlags);
    llvm::cantFail(CPU.setReg(X64Register::AX, VectorLow));
    const auto Before = llvm::cantFail(win::captureUserContext(CPU));
    auto Changed = Original;
    llvm::support::endian::write32le(
        Changed.data() + x64_context::ContextProcessorFlags,
        InitialFlags | (Fault ? 0 : FaultResumeFlag));
    auto Error =
        win::restoreUserContext(CPU, *Snapshot, Original, Changed, StackBase,
                                win::value::StackTop, Origin);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(
        llvm::toString(std::move(Error)).find(win::text::ExceptionContext),
        std::string::npos);
    EXPECT_EQ(llvm::cantFail(win::captureUserContext(CPU)), Before);
    EXPECT_EQ(llvm::cantFail(CPU.readInteger(StackBase, sizeof(uint64_t))),
              VectorHigh);
    llvm::cantFail(win::restoreUserContext(CPU, *Snapshot, Original, Original,
                                           StackBase, win::value::StackTop,
                                           Origin));
    EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::AX)), DataValue);
    EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::FLAGS)), InitialFlags);
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, WindowsExceptions,
                         testing::ValuesIn(Profiles),
                         [](const auto &P) { return P.param.Name; });

TEST(WindowsExceptionsNative, RunsOriginalExceptionExecutable) {
#if !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#elif !defined(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR)
  FAIL() << MissingTools;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.c_str());
  auto Cleanup = llvm::scope_exit(
      [&] { llvm::sys::fs::remove_directories(Root.string()); });
  const auto Program =
      (std::filesystem::path(NEVERD_WINDOWS_EXCEPTION_FIXTURE_DIR) / X64Dir /
       ProgramFile)
          .string();
  const auto Output = (Root / StdoutFile).string();
  const auto Error = (Root / StderrFile).string();
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
    auto Out = llvm::MemoryBuffer::getFile(Output);
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << ObservationLabel << C.Argument << ' ' << Status << ' '
                 << llvm::toHex((*Out)->getBuffer()) << '\n';
    EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
    EXPECT_TRUE((*Err)->getBuffer().empty())
        << llvm::toHex((*Err)->getBuffer());
    EXPECT_EQ(llvm::toHex((*Out)->getBuffer()), C.Expected);
  }
#endif
}
} // namespace
} // namespace neverd::emulation
