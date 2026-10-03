//===- HvfExecutorTests.cpp - Ownership, rollback and native cancellation ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#if defined(__APPLE__) && defined(NEVERD_EMULATION_HVF)
#include "arch/aarch64/AArch64Machine.h"
#include "arch/x86_64/X64Machine.h"
#include "backends/MachineFactories.h"
#include "backends/hvf/HvfExecutor.h"
#include "core/ExecutionDiagnostics.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"

#include <future>
#if defined(__x86_64__)
#include <Hypervisor/hv_vmx.h>
#endif

namespace neverd::emulation {
namespace {
using Clock = std::chrono::steady_clock;
class HvfExecutor : public testing::Test {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::shared_ptr<hvf::Executor> Host;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(4096));
    auto Created = hvf::Executor::acquire();
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Text = llvm::toString(std::move(E));
      if (Unavailable && !std::getenv("NEVERD_REQUIRE_HVF"))
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    Host = *Created;
  }
  MachineRunControl control() {
    return {Clock::now() + std::chrono::seconds(2)};
  }
};
TEST_F(HvfExecutor, OneOwnerThreadAndCancelledQueueAdmissionAllowsRetry) {
  hvf::Binding First(Host, *Memory), Second(Host, *Memory);
  std::promise<void> Entered, Release;
  auto EnteredFuture = Entered.get_future();
  auto ReleaseFuture = Release.get_future();
  std::thread::id Owner, NextOwner;
  std::thread Caller([&] {
    EXPECT_EQ(llvm::toString(First.execute(
                  control(),
                  [&](auto &) -> llvm::Error {
                    Owner = std::this_thread::get_id();
                    Entered.set_value();
                    if (ReleaseFuture.wait_for(std::chrono::seconds(2)) !=
                        std::future_status::ready)
                      return diagnostic::error("test owner was not released");
                    return llvm::Error::success();
                  })),
              "");
  });
  EXPECT_EQ(EnteredFuture.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  std::atomic<bool> Stop{true};
  bool Invoked = false;
  auto E = Second.execute({Clock::now() + std::chrono::seconds(2), &Stop},
                          [&](auto &) -> llvm::Error {
                            Invoked = true;
                            return llvm::Error::success();
                          });
  EXPECT_TRUE(E.isA<MachineInterruptedError>());
  llvm::consumeError(std::move(E));
  EXPECT_FALSE(Invoked);
  Release.set_value();
  Caller.join();
  EXPECT_EQ(llvm::toString(Second.execute(control(),
                                          [&](auto &) -> llvm::Error {
                                            NextOwner =
                                                std::this_thread::get_id();
                                            return llvm::Error::success();
                                          })),
            "");
  EXPECT_EQ(Owner, NextOwner);
  EXPECT_NE(Owner, std::this_thread::get_id());
}
TEST_F(HvfExecutor, PartialMappingFailureRetiresBorrowedBacking) {
  auto Registrations = Memory->registrations();
  // Both regions are valid host allocations, but the second GPA overlaps the
  // first. Exercise native mapping failure after the first mapping succeeds.
  Registrations[1].Physical = Registrations[0].Physical;
  bool Invoked = false;
  auto E =
      Host->execute(this, Registrations, control(), [&](auto &) -> llvm::Error {
        Invoked = true;
        return llvm::Error::success();
      });
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_FALSE(Invoked);
  Memory.reset();
  Host.reset();
  auto Recreated = hvf::Executor::acquire();
  EXPECT_TRUE(bool(Recreated)) << llvm::toString(Recreated.takeError());
}
#if defined(__arm64__)
TEST_F(HvfExecutor,
       GuestFaultDoesNotPublishAtEitherARMPrivilegeAndAllowsRetry) {
  auto Machine = llvm::cantFail(createHvfAArch64Machine(*Memory));
  constexpr uint64_t Code = 0x10000;
  llvm::cantFail(
      Memory->map(Code, 4096, Read | Write | Execute | UserAccessible));
  for (bool User : {false, true}) {
    AArch64MachineState State;
    State.UserMode = User;
    State.reg(AArch64Register::PC) = Code;
    State.reg(AArch64Register::X0) = 0x12345678;
    State.Vectors[31] = {0x1234, 0x5678};
    auto Step = [&](uint32_t Instruction) {
      uint8_t Bytes[4];
      llvm::support::endian::write32le(Bytes, Instruction);
      llvm::cantFail(Memory->write(Code, Bytes));
      llvm::cantFail(Memory->beginRun());
      auto Release = llvm::scope_exit([&] { Memory->endRun(); });
      llvm::cantFail(buildAArch64PageTables(*Memory, User));
      return Machine->step(State, control());
    };
    const auto Before = State;
    auto E = Step(0); // UDF #0 raises an architectural exception.
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
    EXPECT_EQ(State.Registers, Before.Registers);
    EXPECT_EQ(State.Vectors, Before.Vectors);
    EXPECT_EQ(State.UserMode, Before.UserMode);
    EXPECT_EQ(llvm::toString(Step(0xd503201f)), ""); // nop retry
    EXPECT_EQ(State.reg(AArch64Register::PC), Code + 4);
    EXPECT_EQ(State.reg(AArch64Register::X0), Before.reg(AArch64Register::X0));
  }
}
llvm::Error prepareRaw(hvf::Executor &Host, uint64_t PC) {
  const auto CPU = Host.cpu();
  for (auto [Reg, Value] :
       {std::pair{HV_SYS_REG_SCTLR_EL1, uint64_t(0x30d00800)},
        {HV_SYS_REG_MDSCR_EL1, 0},
        {HV_SYS_REG_CNTV_CTL_EL0, 0}})
    if (auto S = hv_vcpu_set_sys_reg(CPU, Reg, Value))
      return hvf::error("test set_sys_reg", S);
  if (auto S = hv_vcpu_set_trap_debug_exceptions(CPU, false))
    return hvf::error("test trap debug", S);
  if (auto S = hv_vcpu_set_reg(CPU, HV_REG_CPSR, 0x3c5))
    return hvf::error("test CPSR", S);
  if (auto S = hv_vcpu_set_reg(CPU, HV_REG_PC, PC))
    return hvf::error("test PC", S);
  return llvm::Error::success();
}
TEST_F(HvfExecutor, NativeLoopDeadlineAndStopRetireInterruptsBeforeRetry) {
  constexpr uint64_t PC = 8192, RetryPC = PC + 64;
  // This raw transport fixture has no guest I-cache maintenance. Keep both
  // programs immutable: replacing the HVC with B at the same PC can execute
  // a stale HVC after migration and falsely report failed cancellation.
  llvm::support::endian::write32le(Memory->data() + PC, 0x14000000); // b .
  llvm::support::endian::write32le(Memory->data() + RetryPC,
                                   0xd4000002); // hvc #0
  hvf::Binding Binding(Host, *Memory);
  for (bool Deadline : {true, false}) {
    SCOPED_TRACE(Deadline ? "deadline" : "stop token");
    std::atomic<bool> Stop{false};
    std::promise<void> Entered;
    auto Ready = Entered.get_future();
    std::thread Stopper;
    if (!Deadline)
      Stopper = std::thread([&] {
        Ready.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Stop = true;
      });
    auto Control = MachineRunControl{
        Clock::now() + std::chrono::milliseconds(Deadline ? 50 : 2000), &Stop};
    bool NativeReturned = false;
    auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
      auto Prepared = prepareRaw(Native, PC);
      Entered.set_value();
      if (Prepared)
        return Prepared;
      return Native.run(Control, [&](bool Cancelled) {
        NativeReturned = true;
        EXPECT_TRUE(Cancelled);
        EXPECT_EQ(Native.exit().reason, HV_EXIT_REASON_CANCELED)
            << "syndrome=" << Native.exit().exception.syndrome;
        return llvm::Error::success();
      });
    });
    if (Ready.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready)
      Entered.set_value();
    if (Stopper.joinable())
      Stopper.join();
    EXPECT_TRUE(NativeReturned);
    EXPECT_TRUE(E.isA<MachineInterruptedError>())
        << llvm::toString(std::move(E));
    llvm::consumeError(std::move(E));
    // Recreated vCPU must enter a different instruction, with no old kick.
    E = Binding.execute(control(), [&](auto &Native) -> llvm::Error {
      if (auto E = prepareRaw(Native, RetryPC))
        return E;
      return Native.run(control(), [&](bool Cancelled) {
        EXPECT_FALSE(Cancelled);
        EXPECT_EQ(Native.exit().reason, HV_EXIT_REASON_EXCEPTION);
        EXPECT_EQ(Native.exit().exception.syndrome >> 26, 0x16u);
        return llvm::Error::success();
      });
    });
    EXPECT_EQ(llvm::toString(std::move(E)), "");
  }
}
TEST_F(HvfExecutor, CompletionFailureOutranksConcurrentStop) {
  constexpr uint64_t PC = 8192;
  llvm::support::endian::write32le(Memory->data() + PC, 0xd4000002);
  hvf::Binding Binding(Host, *Memory);
  std::atomic<bool> Stop{false};
  auto Control = control();
  Control.Stop = &Stop;
  auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
    if (auto E = prepareRaw(Native, PC))
      return E;
    return Native.run(Control, [&](bool) {
      Stop = true;
      return diagnostic::error("injected complete-state capture failure");
    });
  });
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)),
            "injected complete-state capture failure");
}
#endif
#if defined(__x86_64__)
TEST_F(HvfExecutor, NativeIntelCancellationAndCompletionFailureAllowRetry) {
  auto Created = createHvfX64Machine(*Memory);
  ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
  auto Machine = std::move(*Created);
  constexpr uint64_t PC = 0x10000, RetryPC = PC + 2;
  // Keep the loop and retry code immutable. The normal adapter prepares one
  // instruction; only this raw-executor test disables MTF for the busy loop.
  const uint8_t Program[] = {0xeb, 0xfe, 0x90, 0x90}; // jmp .; nop; nop
  ASSERT_EQ(llvm::toString(Memory->map(PC, 4096, Read | Write | Execute)), "");
  ASSERT_EQ(llvm::toString(Memory->write(PC, Program)), "");
  ASSERT_EQ(llvm::toString(Memory->beginRun()), "");
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  auto Root = buildX64PageTables(*Memory);
  ASSERT_TRUE(bool(Root)) << llvm::toString(Root.takeError());
  X64MachineState State;
  State.reg(X64Register::FLAGS) = x64::InitialFlags;
  State.reg(X64Register::AX) = 0x12345678;
  auto Prepare = [&](uint64_t Entry) {
    State.reg(X64Register::PC) = Entry;
    return Machine->step(State, *Root, control());
  };
  hvf::Binding Binding(Host, *Memory);
  enum Mode { Deadline, StopToken, HostInterrupt };
  for (auto Kind : {Deadline, StopToken, HostInterrupt}) {
    SCOPED_TRACE(Kind);
    ASSERT_EQ(llvm::toString(Prepare(PC)), "");
    ASSERT_EQ(State.reg(X64Register::PC), PC);
    std::atomic<bool> Stop{false};
    std::promise<void> Entered;
    auto Ready = Entered.get_future();
    std::optional<hvf::Cpu> InterruptCPU;
    std::thread Stopper;
    if (Kind != Deadline)
      Stopper = std::thread([&] {
        Ready.wait();
        if (Kind == HostInterrupt && InterruptCPU) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          // An unsolicited host kick cannot complete or fail the busy guest.
          // The same entry must continue until the later requested stop.
          EXPECT_EQ(hv_vcpu_interrupt(&*InterruptCPU, 1), HV_SUCCESS);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Stop = true;
      });
    auto Control = MachineRunControl{
        Clock::now() + std::chrono::milliseconds(Kind == Deadline ? 50 : 2000),
        &Stop};
    bool NativeReturned = false;
    auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
      auto Signal = llvm::scope_exit([&] { Entered.set_value(); });
      uint64_t Controls = 0;
      if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED,
                                         &Controls))
        return hvf::error("test read stepping controls", S);
      if (auto S = hv_vmx_vcpu_write_vmcs(Native.cpu(), VMCS_CTRL_CPU_BASED,
                                          Controls & ~uint64_t(CPU_BASED_MTF)))
        return hvf::error("test disable MTF", S);
      InterruptCPU = Native.cpu();
      Signal.release();
      Entered.set_value();
      return Native.run(Control, [&](bool Cancelled) -> llvm::Error {
        NativeReturned = true;
        EXPECT_TRUE(Cancelled);
        uint64_t Reason = 0;
        if (auto S = hv_vmx_vcpu_read_vmcs(Native.cpu(), VMCS_RO_EXIT_REASON,
                                           &Reason))
          return hvf::error("test read interrupted exit", S);
        EXPECT_EQ(Reason, uint64_t(VMX_REASON_IRQ));
        return llvm::Error::success();
      });
    });
    // An admission failure must also release the helper thread.
    if (Ready.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready)
      Entered.set_value();
    if (Stopper.joinable())
      Stopper.join();
    EXPECT_TRUE(NativeReturned);
    EXPECT_TRUE(E.isA<MachineInterruptedError>())
        << llvm::toString(std::move(E));
    llvm::consumeError(std::move(E));
    ASSERT_EQ(llvm::toString(Prepare(RetryPC)), "");
    EXPECT_EQ(State.reg(X64Register::PC), RetryPC + 1);
    EXPECT_EQ(State.reg(X64Register::AX), 0x12345678u);
  }
  std::atomic<bool> Stop{false};
  auto Control = control();
  Control.Stop = &Stop;
  auto E = Binding.execute(Control, [&](auto &Native) -> llvm::Error {
    return Native.run(Control, [&](bool Cancelled) {
      EXPECT_FALSE(Cancelled);
      Stop = true;
      return diagnostic::error("injected Intel complete-state capture failure");
    });
  });
  EXPECT_FALSE(E.isA<MachineInterruptedError>());
  EXPECT_EQ(llvm::toString(std::move(E)),
            "injected Intel complete-state capture failure");
  ASSERT_EQ(llvm::toString(Prepare(RetryPC)), "");
  EXPECT_EQ(State.reg(X64Register::PC), RetryPC + 1);
  EXPECT_EQ(State.reg(X64Register::AX), 0x12345678u);
}
#endif
} // namespace
} // namespace neverd::emulation
#endif
