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

#include "llvm/ADT/ScopeExit.h"

#include <cerrno>
#include <cstdarg>
#include <linux/kvm.h>
#include <sys/ioctl.h>

namespace {
std::atomic<unsigned long> FailureRequest{0};
}

extern "C" int __real_ioctl(int, unsigned long, ...);
// This executable alone injects a single failed read after real guest entry.
// KVM's no-direction requests here use integer arguments; state IO uses
// pointers.
extern "C" int __wrap_ioctl(int FD, unsigned long Request, ...) {
  auto Failure = FailureRequest.load();
  if (Failure == Request &&
      FailureRequest.compare_exchange_strong(Failure, 0)) {
    errno = EIO;
    return -1;
  }
  va_list Arguments;
  va_start(Arguments, Request);
  if (_IOC_DIR(Request) == _IOC_NONE) {
    const int Value = va_arg(Arguments, int);
    va_end(Arguments);
    return __real_ioctl(FD, Request, Value);
  }
  auto *Value = va_arg(Arguments, void *);
  va_end(Arguments);
  return __real_ioctl(FD, Request, Value);
}

namespace neverd::emulation {
namespace {
#if defined(__x86_64__)
#define NEVERD_KVM_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_KVM_STATE_CODE(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "KvmStateTransferCases.def"
#undef NEVERD_KVM_STATE_CODE
#undef NEVERD_KVM_STATE_VALUE
constexpr unsigned long Requests[] = {
#define NEVERD_KVM_STATE_CAPTURE_REQUEST(Request) Request,
#include "KvmStateTransferCases.def"
#undef NEVERD_KVM_STATE_CAPTURE_REQUEST
};
using Parameter = std::tuple<bool, unsigned long>;
class KvmStateTransfer : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  X64MachineState State;
  uint64_t Root = 0;
  void SetUp() override {
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
  llvm::Error step() {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    Root = llvm::cantFail(buildX64PageTables(
        *Memory, Root, false, Machine->requiresExceptionMonitor()));
    return Machine->step(State, Root,
                         {std::chrono::steady_clock::now() +
                          std::chrono::microseconds(Timeout)});
  }
};

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
INSTANTIATE_TEST_SUITE_P(Native, KvmStateTransfer,
                         testing::Combine(testing::Bool(),
                                          testing::ValuesIn(Requests)));
#else
TEST(KvmStateTransfer, NativeX64UnavailableOnThisHost) {
  GTEST_SKIP() << diagnostic::Unavailable;
}
#endif
} // namespace
} // namespace neverd::emulation
