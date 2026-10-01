//===- UnicornStateTransferTests.cpp - Atomic real ARM64 engine capture
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "backends/MachineFactories.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <atomic>
#include <unicorn/unicorn.h>

namespace {
std::atomic<int> FailureRegister{UC_ARM64_REG_INVALID};
}
extern "C" uc_err __real_uc_reg_read(uc_engine *, int, void *);
// Only this executable injects a single failed transport read, after the
// original guest ADD has already changed the real engine's register state.
extern "C" uc_err __wrap_uc_reg_read(uc_engine *Engine, int Register,
                                     void *Value) {
  auto Failure = FailureRegister.load();
  if (Register == Failure &&
      FailureRegister.compare_exchange_strong(Failure, UC_ARM64_REG_INVALID))
    return UC_ERR_RESOURCE;
  return __real_uc_reg_read(Engine, Register, Value);
}

namespace neverd::emulation {
namespace {
#define NEVERD_UNICORN_STATE_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UnicornStateTransferCases.def"
#undef NEVERD_UNICORN_STATE_VALUE

constexpr int RegisterIDs[] = {
#define NEVERD_SCALAR_REGISTER(Arch, Name, Width, Backend)                     \
  NEVERD_REGISTER_##Arch(Backend)
#define NEVERD_REGISTER_X64(Backend)
#define NEVERD_REGISTER_AArch64(Backend) Backend,
#include "neverd/emulation/Registers.def"
#undef NEVERD_REGISTER_AArch64
#undef NEVERD_REGISTER_X64
#undef NEVERD_SCALAR_REGISTER
};
constexpr int VectorIDs[] = {
#define NEVERD_VECTOR_REGISTER(Arch, Index, Backend)                           \
  NEVERD_VECTOR_##Arch(Backend)
#define NEVERD_VECTOR_X64(Backend)
#define NEVERD_VECTOR_AArch64(Backend) Backend,
#include "neverd/emulation/Registers.def"
#undef NEVERD_VECTOR_AArch64
#undef NEVERD_VECTOR_X64
#undef NEVERD_VECTOR_REGISTER
};
class UnicornStateTransfer : public testing::TestWithParam<bool> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<AArch64Machine> Machine;
  AArch64MachineState State;
  void SetUp() override {
    FailureRegister = UC_ARM64_REG_INVALID;
    Memory = llvm::cantFail(MemoryProjection::create(MemoryLimit));
    const unsigned User = GetParam() ? UserAccessible : SupervisorPermission;
    llvm::cantFail(
        Memory->map(Code, memory::PageSize, Read | Write | Execute | User));
    llvm::cantFail(Memory->map(Stack, memory::PageSize, Read | Write | User));
    uint8_t Bytes[aarch64::InstructionBytes];
    llvm::support::endian::write32le(Bytes, IncrementInteger);
    llvm::cantFail(Memory->write(Code, Bytes));
    auto Created = createUnicornAArch64Machine(*Memory, GetParam());
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Machine = std::move(*Created);
    State.UserMode = GetParam();
    for (unsigned I = 0; I < State.Registers.size(); ++I)
      State.Registers[I] = InitialScalar + I;
    State.reg(AArch64Register::X0) = InitialInteger;
    State.reg(AArch64Register::PC) = Code;
    State.reg(AArch64Register::SP) = Stack + memory::PageSize - StackAlignment;
    State.reg(AArch64Register::NZCV) = PoisonedNZCV;
    State.reg(AArch64Register::FPCR) = PoisonedFPControl;
    State.reg(AArch64Register::FPSR) = PoisonedFPControl;
    State.Vectors.fill({InitialVectorLow, InitialVectorHigh});
  }
  void TearDown() override { FailureRegister = UC_ARM64_REG_INVALID; }
  llvm::Error step() {
    if (auto E = Memory->beginRun())
      return E;
    auto Release = llvm::scope_exit([&] { Memory->endRun(); });
    if (auto E = buildAArch64PageTables(*Memory, GetParam()))
      return E;
    return Machine->step(State, {});
  }
  void expectCaptured(const AArch64MachineState &Before) {
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    for (unsigned I = 0; I < State.Registers.size(); ++I) {
      const auto Register = AArch64Register(I);
      const auto Expected =
          Register == AArch64Register::X0   ? InitialInteger + IntegerIncrement
          : Register == AArch64Register::PC ? Code + aarch64::InstructionBytes
          : Register == AArch64Register::NZCV ? ExpectedNZCV
          : Register == AArch64Register::FPCR ||
                  Register == AArch64Register::FPSR
              ? ExpectedFPControl
              : Before.reg(Register);
      EXPECT_EQ(State.reg(Register), Expected) << I;
    }
  }
};
TEST_P(UnicornStateTransfer, FailedScalarReadRetainsAllInputAndAllowsRetry) {
  const auto Before = State;
  ASSERT_EQ(std::size(RegisterIDs), unsigned(AArch64Register::FPSR) + 1);
  for (int Register : RegisterIDs) {
    SCOPED_TRACE(Register);
    State = Before;
    FailureRegister = Register;
    EXPECT_EQ(llvm::toString(step()), uc_strerror(UC_ERR_RESOURCE));
    EXPECT_EQ(FailureRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    // The engine already advanced. Retrying must install the untouched input
    // and execute the original instruction exactly once.
    ASSERT_EQ(llvm::toString(step()), "");
    expectCaptured(Before);
  }
}
TEST_P(UnicornStateTransfer, CapturesDeclaredWidthsWithoutStaleUpperBits) {
  const auto Before = State;
  ASSERT_EQ(llvm::toString(step()), "");
  expectCaptured(Before);
}
TEST_P(UnicornStateTransfer, FailedVectorReadCannotPublishScalarOrVectorState) {
  const auto Before = State;
  for (int Register : VectorIDs) {
    SCOPED_TRACE(Register);
    State = Before;
    FailureRegister = Register;
    EXPECT_EQ(llvm::toString(step()), uc_strerror(UC_ERR_RESOURCE));
    EXPECT_EQ(FailureRegister.load(), UC_ARM64_REG_INVALID);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    ASSERT_EQ(llvm::toString(step()), "");
    expectCaptured(Before);
  }
}
INSTANTIATE_TEST_SUITE_P(Privileges, UnicornStateTransfer, testing::Bool());
} // namespace
} // namespace neverd::emulation
