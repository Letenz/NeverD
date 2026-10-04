//===- KvmStateTransferTests.cpp - Native state after capture failure -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64ExceptionMonitor.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <cerrno>
#include <cstdarg>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <vector>

namespace {
std::atomic<unsigned long> FailureRequest{0};
std::atomic<unsigned long> CancellationRequest{0};
std::atomic<neverd::emulation::ExecutionBackend *> PublicCPU{nullptr};
std::atomic<std::atomic<bool> *> PrivateStop{nullptr};
#if defined(__x86_64__)
std::atomic<int> AllowedSync{0}, AvailableSync{0};
std::atomic<unsigned> GeneralReads{0}, SpecialReads{0}, Entries{0};
#endif
} // namespace

extern "C" int __real_ioctl(int, unsigned long, ...);
// This executable alone injects a single failed read after real guest entry.
// KVM's no-direction requests here use integer arguments; state IO uses
// pointers.
extern "C" int __wrap_ioctl(int FD, unsigned long Request, ...) {
  auto Failure = FailureRequest.load();
  if (Failure == Request &&
      FailureRequest.compare_exchange_strong(Failure, 0)) {
    if (auto *CPU = PublicCPU.load())
      CPU->stop();
    errno = EIO;
    return -1;
  }
  va_list Arguments;
  va_start(Arguments, Request);
  if (_IOC_DIR(Request) == _IOC_NONE) {
    const int Value = va_arg(Arguments, int);
    va_end(Arguments);
#if defined(__x86_64__)
    if (Request == KVM_CHECK_EXTENSION && Value == KVM_CAP_SYNC_REGS) {
      if (AllowedSync < 0) {
        errno = EIO;
        return -1;
      }
      const int Result = __real_ioctl(FD, Request, Value);
      AvailableSync = Result > 0 ? Result & AllowedSync : 0;
      return AvailableSync;
    }
    if (Request == KVM_RUN)
      ++Entries;
#endif
    return __real_ioctl(FD, Request, Value);
  }
  auto *Value = va_arg(Arguments, void *);
  va_end(Arguments);
#if defined(__x86_64__)
  if (Request == KVM_GET_REGS)
    ++GeneralReads;
  if (Request == KVM_GET_SREGS)
    ++SpecialReads;
#endif
  const int Result = __real_ioctl(FD, Request, Value);
  auto Cancel = CancellationRequest.load();
  if (Result >= 0 && Cancel == Request &&
      CancellationRequest.compare_exchange_strong(Cancel, 0)) {
    if (auto *CPU = PublicCPU.load())
      CPU->stop();
    if (auto *Stop = PrivateStop.load())
      *Stop = true;
  }
  return Result;
}

namespace neverd::emulation {
namespace {
#if defined(__x86_64__)
#define NEVERD_KVM_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_KVM_STATE_CODE(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_KVM_STATE_TEXT(Name, Text) constexpr char Name[] = Text;
#include "KvmStateTransferCases.def"
#undef NEVERD_KVM_STATE_TEXT
#undef NEVERD_KVM_STATE_CODE
#undef NEVERD_KVM_STATE_VALUE
constexpr unsigned long Requests[] = {
#define NEVERD_KVM_STATE_CAPTURE_REQUEST(Request) Request,
#include "KvmStateTransferCases.def"
#undef NEVERD_KVM_STATE_CAPTURE_REQUEST
};
constexpr int SyncModes[] = {
#define NEVERD_KVM_STATE_SYNC_MODE(Mask) Mask,
#include "KvmStateTransferCases.def"
#undef NEVERD_KVM_STATE_SYNC_MODE
};
using Parameter = std::tuple<bool, unsigned long, int>;
std::vector<Parameter> parameters() {
  std::vector<Parameter> Result;
  for (bool Vector : {false, true})
    for (auto Request : Requests)
      for (int Mode : SyncModes) {
        // A synchronized GPR capture deliberately makes no GET_REGS call.
        if (Mode > 0 && (Mode & KVM_SYNC_X86_REGS) && Request == KVM_GET_REGS)
          continue;
        Result.emplace_back(Vector, Request, Mode);
      }
  return Result;
}
class KvmStateTransfer : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState State;
  uint64_t Root = 0;
  void SetUp() override {
    FailureRequest = CancellationRequest = 0;
    PublicCPU = nullptr;
    PrivateStop = nullptr;
    AllowedSync = std::get<2>(GetParam());
    AvailableSync = 0;
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto Created = createKvmMachine(*Memory);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*Created);
    if (AllowedSync > 0 && AvailableSync.load() != AllowedSync.load())
      GTEST_SKIP() << SyncUnavailable;
    llvm::cantFail(Memory->map(Code, PageSize, Read | Write | Execute));
    llvm::cantFail(Memory->map(Data, PageSize, Read | Write));
    llvm::cantFail(Memory->write(Code, Warm));
    const auto Body = std::get<0>(GetParam())
                          ? llvm::ArrayRef<uint8_t>(IncrementVector)
                          : llvm::ArrayRef<uint8_t>(IncrementInteger);
    llvm::cantFail(Memory->write(Code + sizeof(Warm), Body));
    llvm::cantFail(Memory->write(Code + sizeof(Warm) + Body.size(), Warm));
    State.reg(X64Register::PC) = Code;
    State.reg(X64Register::SP) = Data + PageSize - WordBytes;
    State.reg(X64Register::FLAGS) = InitialFlags;
    State.reg(X64Register::AX) = InitialInteger;
    State.Xmm.front() =
        State.Xmm[IncrementVectorIndex] = {InitialPacked, InitialPacked};
  }
  void TearDown() override {
    Machine.reset();
    AllowedSync = AvailableSync = 0;
  }
  llvm::Error step(const std::atomic<bool> *Stop = nullptr) {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    Root = llvm::cantFail(buildX64PageTables(
        *Memory, State.UserMode, Machine->requiresExceptionMonitor()));
    return Machine->step(
        State, Root,
        {std::chrono::steady_clock::now() + std::chrono::microseconds(Timeout),
         Stop});
  }
};

TEST_P(KvmStateTransfer, SynchronizedCapturesRemoveOnlySupportedReadIoctls) {
  std::array<uint8_t, RepeatedSteps> Nops;
  Nops.fill(Warm[0]);
  llvm::cantFail(Memory->write(Code, Nops));
  ASSERT_EQ(llvm::toString(step()), "");
  GeneralReads = SpecialReads = Entries = 0;
  const auto Before = State;
  for (unsigned Index = 1; Index < RepeatedSteps; ++Index)
    ASSERT_EQ(llvm::toString(step()), "");
  EXPECT_EQ(Entries.load(), RepeatedSteps - 1);
  EXPECT_EQ(GeneralReads.load(),
            AvailableSync & KVM_SYNC_X86_REGS ? 0 : RepeatedSteps - 1);
  EXPECT_EQ(SpecialReads.load(),
            AvailableSync & KVM_SYNC_X86_SREGS ? 0 : RepeatedSteps - 1);
  auto Expected = Before;
  Expected.reg(X64Register::PC) = Code + RepeatedSteps;
  EXPECT_EQ(State, Expected);
}

TEST_P(KvmStateTransfer, CancelledWarmEntryRequiresFreshSpecialStateOnRetry) {
  ASSERT_EQ(llvm::toString(step()), "");
  const auto Before = State;
  std::atomic<bool> Stop{true};
  auto E = step(&Stop);
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  llvm::consumeError(std::move(E));
  EXPECT_EQ(State, Before);
  SpecialReads = 0;
  Stop = false;
  ASSERT_EQ(llvm::toString(step(&Stop)), "");
  EXPECT_EQ(SpecialReads.load(), 1u);
  const bool Vector = std::get<0>(GetParam());
  EXPECT_EQ(State.reg(X64Register::AX),
            InitialInteger + (Vector ? 0 : IntegerIncrement));
  const auto Packed = Vector ? IncrementedPacked : InitialPacked;
  EXPECT_EQ(State.Xmm.front(), (ExecutionBackend::XmmValue{Packed, Packed}));
}

TEST_P(KvmStateTransfer, FailedReadRetainsInputAndReinstallsItBeforeRetry) {
  ASSERT_EQ(llvm::toString(step()), "");
  const auto Before = State;
  FailureRequest = std::get<1>(GetParam());
  EXPECT_EQ(llvm::toString(step()), diagnostic::KvmState);
  EXPECT_EQ(FailureRequest.load(), 0u);
  EXPECT_EQ(State.Registers, Before.Registers);
  EXPECT_EQ(State.Xmm, Before.Xmm);
  EXPECT_EQ(State.MXCSR, Before.MXCSR);
  EXPECT_EQ(State.FP.Registers, Before.FP.Registers);
  // The native CPU already changed, but the owner still holds the old input.
  ASSERT_EQ(llvm::toString(step()), "");
  const bool Vector = std::get<0>(GetParam());
  EXPECT_EQ(State.reg(X64Register::PC),
            Code + sizeof(Warm) +
                (Vector ? sizeof(IncrementVector) : sizeof(IncrementInteger)));
  EXPECT_EQ(State.reg(X64Register::AX),
            InitialInteger + (Vector ? 0 : IntegerIncrement));
  const auto Packed = Vector ? IncrementedPacked : InitialPacked;
  EXPECT_EQ(State.Xmm.front(), (ExecutionBackend::XmmValue{Packed, Packed}));
}
TEST_P(KvmStateTransfer, PublicCancellationRetainsStateRAMAndFailurePriority) {
  auto Created = createExecutionBackend(ExecutionBackendKind::KVM,
                                        ExecutionContract::CheckedX64, Limit);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto CPU = std::move(Created->CPU);
  llvm::cantFail(CPU->map(Code, PageSize, Read | Write | Execute));
  llvm::cantFail(CPU->map(Data, PageSize, Read | Write));
  llvm::cantFail(CPU->write(Code, StoreInteger));
  llvm::cantFail(CPU->write(Code + sizeof(StoreInteger), Warm));
  llvm::cantFail(CPU->writeInteger(Data, InitialInteger, WordBytes));
  llvm::cantFail(
      CPU->setReg(X64Register::AX, InitialInteger + IntegerIncrement));
  llvm::cantFail(CPU->setReg(X64Register::DI, Data));
  PublicCPU = CPU.get();
  auto Release = llvm::scope_exit([&] {
    PublicCPU = nullptr;
    FailureRequest = CancellationRequest = 0;
  });
  const bool Failed = std::get<0>(GetParam());
  const auto Request = std::get<1>(GetParam());
  if (Failed)
    FailureRequest = Request;
  else
    CancellationRequest = Request;
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  ASSERT_EQ(Exit.Kind, Failed ? ExecutionExitKind::BackendFailure
                              : ExecutionExitKind::Stopped);
  EXPECT_TRUE(Exit.StopRequested);
  EXPECT_FALSE(Exit.DeadlineReached);
  EXPECT_EQ(bool(Exit.Fault), Failed);
  EXPECT_EQ(FailureRequest.load(), 0u);
  EXPECT_EQ(CancellationRequest.load(), 0u);
  uint8_t Original[WordBytes]{};
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data, Original)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Original), InitialInteger);
  if (Failed) {
    EXPECT_EQ(Exit.Diagnostic, diagnostic::KvmState);
    return;
  }
  EXPECT_TRUE(Exit.Diagnostic.empty());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC != Code)
      CPU->stop();
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Retry = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Retry.Kind, ExecutionExitKind::Stopped);
  EXPECT_FALSE(Retry.Fault);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)),
            InitialInteger + IntegerIncrement);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
            Code + sizeof(StoreInteger));
}
TEST_P(KvmStateTransfer, ActualCPUExceptionOutranksStopDuringCapture) {
  State.UserMode = std::get<0>(GetParam());
  llvm::cantFail(
      Memory->protect(Code, PageSize, Read | Write | Execute | UserAccessible));
  llvm::cantFail(
      Memory->protect(Data, PageSize, Read | Write | UserAccessible));
  llvm::cantFail(Memory->write(Code + sizeof(Warm), DivideInteger));
  State.reg(X64Register::CX) = 0;
  ASSERT_EQ(llvm::toString(step()), "");
  const auto Before = State;
  std::atomic<bool> Stop{false};
  PrivateStop = &Stop;
  CancellationRequest = std::get<1>(GetParam());
  auto Release = llvm::scope_exit([&] {
    PrivateStop = nullptr;
    CancellationRequest = 0;
  });
  bool Caught = false;
  auto Remaining =
      llvm::handleErrors(step(&Stop), [&](const X64ExceptionError &E) {
        Caught = true;
        EXPECT_EQ(E.exception().Vector, unsigned(x64::ExceptionVector::Divide));
        EXPECT_FALSE(E.exception().ErrorCode);
        EXPECT_FALSE(E.exception().FaultAddress);
      });
  EXPECT_EQ(llvm::toString(std::move(Remaining)), "");
  EXPECT_TRUE(Caught);
  EXPECT_TRUE(Stop);
  EXPECT_EQ(CancellationRequest.load(), 0u);
  EXPECT_EQ(State.Registers, Before.Registers);
  EXPECT_EQ(State.Xmm, Before.Xmm);
  EXPECT_EQ(State.MXCSR, Before.MXCSR);
  EXPECT_EQ(State.FP.Registers, Before.FP.Registers);
  EXPECT_EQ(State.GSBase, Before.GSBase);
  EXPECT_EQ(State.FSBase, Before.FSBase);
  EXPECT_EQ(State.UserMode, Before.UserMode);
  // Replace the divisor explicitly. A raw machine has no terminal OS policy.
  Stop = false;
  State.reg(X64Register::CX) = IntegerIncrement;
  ASSERT_EQ(llvm::toString(step(&Stop)), "");
  EXPECT_EQ(State.reg(X64Register::PC),
            Code + sizeof(Warm) + sizeof(DivideInteger));
  EXPECT_EQ(State.reg(X64Register::AX), InitialInteger);
  EXPECT_EQ(State.reg(X64Register::DX), 0u);
}
TEST_P(KvmStateTransfer, PublicCPUExceptionOutranksStopDuringCapture) {
  auto Created = createExecutionBackend(ExecutionBackendKind::KVM,
                                        ExecutionContract::CheckedX64, Limit);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto CPU = std::move(Created->CPU);
  llvm::cantFail(CPU->map(Code, PageSize, Read | Write | Execute));
  llvm::cantFail(CPU->map(Data, PageSize, Read | Write));
  llvm::cantFail(CPU->write(Code, DivideInteger));
  llvm::cantFail(CPU->writeInteger(Data, InitialInteger, WordBytes));
  llvm::cantFail(CPU->setReg(X64Register::AX, InitialInteger));
  llvm::cantFail(CPU->setReg(X64Register::SP, Data + PageSize - WordBytes));
  const bool Recoverable = std::get<0>(GetParam());
  bool Observed = false;
  BackendHooks Hooks;
  if (Recoverable)
    Hooks.RecoverableFault = [&](const BackendFault &Fault) {
      Observed = true;
      EXPECT_EQ(Fault.Kind, BackendFaultKind::Interrupt);
      EXPECT_EQ(Fault.Interrupt, unsigned(x64::ExceptionVector::Divide));
      return true;
    };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  PublicCPU = CPU.get();
  CancellationRequest = std::get<1>(GetParam());
  auto Release = llvm::scope_exit([&] {
    PublicCPU = nullptr;
    CancellationRequest = 0;
  });
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, Recoverable ? ExecutionExitKind::RecoverableFault
                                   : ExecutionExitKind::GuestTrap);
  EXPECT_TRUE(Exit.StopRequested);
  EXPECT_FALSE(Exit.DeadlineReached);
  EXPECT_EQ(CancellationRequest.load(), 0u);
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::Interrupt);
  EXPECT_EQ(Exit.Fault->PC, Code);
  EXPECT_EQ(Exit.Fault->Interrupt, unsigned(x64::ExceptionVector::Divide));
  EXPECT_EQ(Observed, Recoverable);
  uint8_t Original[WordBytes]{};
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data, Original)), "");
  EXPECT_EQ(llvm::support::endian::read64le(Original), InitialInteger);
  if (Recoverable) {
    auto Taken = CPU->takeRecoverableFault();
    ASSERT_TRUE(Taken);
    llvm::cantFail(CPU->setReg(X64Register::CX, IntegerIncrement));
    BackendHooks RetryHooks;
    RetryHooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(RetryHooks)));
    auto Retry = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Retry.Kind, ExecutionExitKind::Stopped);
    EXPECT_FALSE(Retry.Fault);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
              Code + sizeof(DivideInteger));
  }
}
INSTANTIATE_TEST_SUITE_P(Native, KvmStateTransfer,
                         testing::ValuesIn(parameters()));
#else
TEST(KvmStateTransfer, NativeX64UnavailableOnThisHost) {
  GTEST_SKIP() << diagnostic::Unavailable;
}
#endif
} // namespace
} // namespace neverd::emulation
