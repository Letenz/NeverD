//===- UnicornMachineControlTests.cpp - Checked software entry control ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "arch/x86_64/X64Exception.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <atomic>
#include <thread>
#include <tuple>
#include <unicorn/unicorn.h>

namespace {
using namespace neverd::emulation;
#define NEVERD_UNICORN_CONTROL_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#define NEVERD_UNICORN_CONTROL_INSTRUCTION(Name, ...)                          \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "UnicornMachineControlCases.def"
#undef NEVERD_UNICORN_CONTROL_INSTRUCTION
#undef NEVERD_UNICORN_CONTROL_VALUE
enum class Injection {
  None,
  BeforeStep,
  BeforeGuest,
  ExpiredGuest,
  DuringCapture,
  ExpiredCapture,
  FailedCapture
};
Injection Inject;
std::atomic<bool> Stop;
unsigned GuestEntries;
int CaptureRegister;
ExecutionBackend *PublicCPU;
} // namespace

extern "C" uc_err __real_uc_emu_start(uc_engine *, uint64_t, uint64_t, uint64_t,
                                      size_t);
// Only this executable can request a stop or expire the borrowed deadline
// immediately before the real engine enters the original guest instruction.
extern "C" uc_err __wrap_uc_emu_start(uc_engine *Engine, uint64_t Begin,
                                      uint64_t End, uint64_t Timeout,
                                      size_t Count) {
  if (Begin == Code) {
    ++GuestEntries;
    if (Inject == Injection::BeforeGuest)
      Stop = true;
    if (Inject == Injection::ExpiredGuest)
      std::this_thread::sleep_for(std::chrono::microseconds(
          ExpiryDelayMultiplier *
          execution_limits::NativeStepGraceMicroseconds));
  }
  return __real_uc_emu_start(Engine, Begin, End, Timeout, Count);
}
extern "C" uc_err __real_uc_reg_read(uc_engine *, int, void *);
extern "C" uc_err __wrap_uc_reg_read(uc_engine *Engine, int Register,
                                     void *Value) {
  const auto Result = __real_uc_reg_read(Engine, Register, Value);
  if (Register == CaptureRegister && GuestEntries &&
      (Inject == Injection::DuringCapture ||
       Inject == Injection::ExpiredCapture ||
       Inject == Injection::FailedCapture)) {
    const bool Failed = Inject == Injection::FailedCapture;
    if (Inject == Injection::ExpiredCapture)
      std::this_thread::sleep_for(std::chrono::microseconds(
          ExpiryDelayMultiplier *
          execution_limits::NativeStepGraceMicroseconds));
    else {
      Stop = true;
      if (PublicCPU)
        PublicCPU->stop();
    }
    Inject = Injection::None;
    if (Failed)
      return UC_ERR_RESOURCE;
  }
  return Result;
}

namespace neverd::emulation {
namespace {
using Case = std::tuple<GuestArchitecture, bool, Injection>;
class UnicornMachineControl : public testing::TestWithParam<Case> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> X64;
  std::unique_ptr<AArch64Machine> ARM;
  X64MachineState X64State;
  AArch64MachineState ARMState;
  uint64_t Root = 0;
  bool user() const { return std::get<1>(GetParam()); }
  bool arm() const {
    return std::get<0>(GetParam()) == GuestArchitecture::AArch64;
  }
  void SetUp() override {
    Inject = Injection::None;
    Stop = false;
    PublicCPU = nullptr;
    Memory = llvm::cantFail(MemoryProjection::create(MemoryLimit));
    const unsigned User = user() ? UserAccessible : SupervisorPermission;
    llvm::cantFail(
        Memory->map(Code, memory::PageSize, Read | Write | Execute | User));
    llvm::cantFail(Memory->map(Stack, memory::PageSize, Read | Write | User));
    llvm::cantFail(Memory->map(Data, memory::PageSize, Read | Write | User));
    llvm::cantFail(
        Memory->write(Code, arm() ? llvm::ArrayRef<uint8_t>(AArch64Store)
                                  : llvm::ArrayRef<uint8_t>(X64Store)));
    uint8_t Bytes[sizeof(InitialData)];
    llvm::support::endian::write64le(Bytes, InitialData);
    llvm::cantFail(Memory->write(Data, Bytes));
    if (arm()) {
      auto Created = createUnicornAArch64Machine(*Memory, user());
      ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
      ARM = std::move(*Created);
      ARMState.UserMode = user();
      ARMState.reg(AArch64Register::X0) = StoredData;
      ARMState.reg(AArch64Register::X1) = Data;
      ARMState.reg(AArch64Register::PC) = Code;
      ARMState.reg(AArch64Register::SP) =
          Stack + memory::PageSize - StackAlignment;
    } else {
      auto Created = createUnicornX64Machine(*Memory, user());
      ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
      X64 = std::move(*Created);
      X64State.UserMode = user();
      X64State.reg(X64Register::AX) = StoredData;
      X64State.reg(X64Register::DI) = Data;
      X64State.reg(X64Register::PC) = Code;
      X64State.reg(X64Register::SP) = Stack + memory::PageSize - StackAlignment;
      X64State.reg(X64Register::FLAGS) = x64::InitialFlags;
    }
    GuestEntries = 0;
    CaptureRegister = arm() ? int(UC_ARM64_REG_Q31) : int(UC_X86_REG_FPTAG);
  }
  void TearDown() override {
    Inject = Injection::None;
    Stop = false;
    PublicCPU = nullptr;
  }
  llvm::Error step(MachineRunControl Control) {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    if (arm()) {
      if (auto E = buildAArch64PageTables(*Memory, user()))
        return E;
      return ARM->step(ARMState, Control);
    }
    auto Built = buildX64PageTables(*Memory, Root, user());
    if (!Built)
      return Built.takeError();
    Root = *Built;
    return X64->step(X64State, Root, Control);
  }
  uint64_t data() const {
    uint8_t Bytes[sizeof(InitialData)];
    llvm::cantFail(Memory->read(Data, Bytes));
    return llvm::support::endian::read64le(Bytes);
  }
  void expectUnchanged(const X64MachineState &X64Before,
                       const AArch64MachineState &ARMBefore) {
    EXPECT_EQ(ARMState.UserMode, ARMBefore.UserMode);
    EXPECT_EQ(ARMState.Registers, ARMBefore.Registers);
    EXPECT_EQ(ARMState.Vectors, ARMBefore.Vectors);
    EXPECT_EQ(X64State.UserMode, X64Before.UserMode);
    EXPECT_EQ(X64State.Registers, X64Before.Registers);
    EXPECT_EQ(X64State.Xmm, X64Before.Xmm);
    EXPECT_EQ(X64State.GSBase, X64Before.GSBase);
    EXPECT_EQ(X64State.FSBase, X64Before.FSBase);
    EXPECT_EQ(X64State.MXCSR, X64Before.MXCSR);
#define NEVERD_X64_FP_CONTROL(Name, Member, Type, UC, Offset)                  \
  EXPECT_EQ(X64State.FP.Member, X64Before.FP.Member);
#include "arch/x86_64/X64FPState.def"
#undef NEVERD_X64_FP_CONTROL
    EXPECT_EQ(X64State.FP.Tag, X64Before.FP.Tag);
    EXPECT_EQ(X64State.FP.Registers, X64Before.FP.Registers);
  }
};

TEST_P(UnicornMachineControl, RejectedEntryPreservesStateAndRAMAndAllowsRetry) {
  const auto X64Before = X64State;
  const auto ARMBefore = ARMState;
  Inject = std::get<2>(GetParam());
  Stop = Inject == Injection::BeforeStep;
  EXPECT_EQ(llvm::toString(step({{}, &Stop})), diagnostic::UnicornRun);
  EXPECT_EQ(data(), InitialData);
  EXPECT_EQ(GuestEntries, Inject == Injection::BeforeStep ? 0u : 1u);
  expectUnchanged(X64Before, ARMBefore);
  Inject = Injection::None;
  Stop = false;
  ASSERT_EQ(
      llvm::toString(step({std::chrono::steady_clock::now() +
                               std::chrono::microseconds(RetryMicroseconds),
                           &Stop})),
      "");
  EXPECT_EQ(data(), StoredData);
  EXPECT_EQ(GuestEntries,
            (std::get<2>(GetParam()) == Injection::BeforeStep ? 0u : 1u) + 1u);
  EXPECT_EQ(arm() ? ARMState.reg(AArch64Register::PC)
                  : X64State.reg(X64Register::PC),
            Code + (arm() ? sizeof(AArch64Store) : sizeof(X64Store)));
}

INSTANTIATE_TEST_SUITE_P(
    Architectures, UnicornMachineControl,
    testing::Combine(
        testing::Values(GuestArchitecture::X64, GuestArchitecture::AArch64),
        testing::Bool(),
        testing::Values(Injection::BeforeStep, Injection::BeforeGuest,
                        Injection::ExpiredGuest)));
class UnicornCaptureControl : public UnicornMachineControl {};
TEST_P(UnicornCaptureControl, CompletedStoreCannotPublishCancelledCapture) {
  const auto X64Before = X64State;
  const auto ARMBefore = ARMState;
  Inject = std::get<2>(GetParam());
  EXPECT_EQ(llvm::toString(step({{}, &Stop})), diagnostic::UnicornRun);
  ASSERT_EQ(Inject, Injection::None);
  EXPECT_EQ(GuestEntries, 1u);
  // The actual guest store preceded cancellation. Checked execution owns the
  // RAM transaction; this machine boundary must still withhold CPU state.
  EXPECT_EQ(data(), StoredData);
  expectUnchanged(X64Before, ARMBefore);
  Stop = false;
  ASSERT_EQ(
      llvm::toString(step({std::chrono::steady_clock::now() +
                               std::chrono::microseconds(RetryMicroseconds),
                           &Stop})),
      "");
  EXPECT_EQ(GuestEntries, 2u);
  EXPECT_EQ(arm() ? ARMState.reg(AArch64Register::PC)
                  : X64State.reg(X64Register::PC),
            Code + (arm() ? sizeof(AArch64Store) : sizeof(X64Store)));
}
INSTANTIATE_TEST_SUITE_P(
    Architectures, UnicornCaptureControl,
    testing::Combine(
        testing::Values(GuestArchitecture::X64, GuestArchitecture::AArch64),
        testing::Bool(),
        testing::Values(Injection::DuringCapture, Injection::ExpiredCapture)));

class UnicornExceptionCapture : public UnicornMachineControl {};
TEST_P(UnicornExceptionCapture, StopDuringCaptureCannotHideRealGuestException) {
  llvm::cantFail(Memory->write(Code, X64InvalidOpcode));
  Inject = Injection::DuringCapture;
  auto Error = step({{}, &Stop});
  EXPECT_TRUE(bool(Error));
  bool Observed = false;
  llvm::handleAllErrors(
      std::move(Error),
      [&](const X64ExceptionError &E) {
        Observed = true;
        EXPECT_EQ(E.exception().Vector,
                  unsigned(x64::ExceptionVector::InvalidOpcode));
      },
      [&](const llvm::ErrorInfoBase &E) { ADD_FAILURE() << E.message(); });
  EXPECT_TRUE(Observed);
  EXPECT_TRUE(Stop);
  EXPECT_EQ(GuestEntries, 1u);
  EXPECT_EQ(X64State.reg(X64Register::PC), Code);
  EXPECT_EQ(data(), InitialData);
}
INSTANTIATE_TEST_SUITE_P(
    Privileges, UnicornExceptionCapture,
    testing::Combine(testing::Values(GuestArchitecture::X64), testing::Bool(),
                     testing::Values(Injection::DuringCapture)));

class UnicornPublicCapture : public UnicornMachineControl {};
TEST_P(UnicornPublicCapture, CancellationKeepsTypedExitStateAndRAMConsistent) {
  const auto ISA = std::get<0>(GetParam());
  const auto Contract = arm() ? (user() ? ExecutionContract::CheckedUserAArch64
                                        : ExecutionContract::CheckedAArch64)
                              : (user() ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64);
  auto CPU =
      llvm::cantFail(createExecutionBackend(ExecutionBackendKind::Unicorn,
                                            Contract, MemoryLimit, ISA))
          .CPU;
  const unsigned User = user() ? UserAccessible : SupervisorPermission;
  llvm::cantFail(
      CPU->map(Code, memory::PageSize, Read | Write | Execute | User));
  llvm::cantFail(CPU->map(Data, memory::PageSize, Read | Write | User));
  const auto Bytes = arm() ? llvm::ArrayRef<uint8_t>(AArch64Store)
                           : llvm::ArrayRef<uint8_t>(X64Store);
  llvm::cantFail(CPU->write(Code, Bytes));
  llvm::cantFail(CPU->write(Code + Bytes.size(), Bytes));
  llvm::cantFail(CPU->writeInteger(Data, InitialData, sizeof(InitialData)));
  llvm::cantFail(CPU->writeRegister(
      arm() ? CPURegister::AArch64X0 : CPURegister::X64AX, {StoredData, 0}));
  llvm::cantFail(CPU->writeRegister(
      arm() ? CPURegister::AArch64X1 : CPURegister::X64DI, {Data, 0}));
  PublicCPU = CPU.get();
  auto Release = llvm::scope_exit([&] { PublicCPU = nullptr; });
  GuestEntries = 0;
  const auto Mode = std::get<2>(GetParam());
  Inject = Mode;
  auto Exit = llvm::cantFail(CPU->runUntilExit(
      Code, Mode == Injection::ExpiredCapture
                ? execution_limits::NativeStepGraceMicroseconds
                : RetryMicroseconds));
  ASSERT_EQ(Inject, Injection::None);
  EXPECT_EQ(GuestEntries, 1u);
  const auto Expected =
      Mode == Injection::FailedCapture    ? ExecutionExitKind::BackendFailure
      : Mode == Injection::ExpiredCapture ? ExecutionExitKind::Deadline
                                          : ExecutionExitKind::Stopped;
  ASSERT_EQ(Exit.Kind, Expected);
  EXPECT_EQ(Exit.StopRequested, Mode != Injection::ExpiredCapture);
  EXPECT_EQ(Exit.DeadlineReached, Mode == Injection::ExpiredCapture);
  EXPECT_EQ(bool(Exit.Fault), Mode == Injection::FailedCapture);
  uint8_t Original[sizeof(InitialData)]{};
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data, Original)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Original), InitialData);
  if (Mode == Injection::FailedCapture) {
    EXPECT_EQ(Exit.Diagnostic, uc_strerror(UC_ERR_RESOURCE));
    return;
  }
  EXPECT_TRUE(Exit.Diagnostic.empty());
  const auto PC = arm() ? CPURegister::AArch64PC : CPURegister::X64PC;
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(PC))[0], Code);
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t Address, uint32_t) {
    if (Address != Code)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Retry = llvm::cantFail(CPU->runUntilExit(Code, RetryMicroseconds));
  EXPECT_EQ(Retry.Kind, ExecutionExitKind::Stopped);
  EXPECT_FALSE(Retry.Fault);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, sizeof(StoredData))),
            StoredData);
  EXPECT_EQ(llvm::cantFail(CPU->readRegister(PC))[0], Code + Bytes.size());
}
INSTANTIATE_TEST_SUITE_P(
    Architectures, UnicornPublicCapture,
    testing::Combine(
        testing::Values(GuestArchitecture::X64, GuestArchitecture::AArch64),
        testing::Bool(),
        testing::Values(Injection::DuringCapture, Injection::ExpiredCapture,
                        Injection::FailedCapture)));
} // namespace
} // namespace neverd::emulation
