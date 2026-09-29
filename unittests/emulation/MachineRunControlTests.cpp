//===- MachineRunControlTests.cpp - CPU to transport cancellation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/CheckedAArch64Backend.h"
#include "arch/x86_64/CheckedX64Backend.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <condition_variable>
#include <future>
#include <mutex>

namespace neverd::emulation {
namespace {
#define NEVERD_CONFIGURATION_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_CONFIGURATION_X64(Name, ...)                                    \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_CONFIGURATION_ARM(Name, ...)                                    \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "ExecutionConfigurationCases.def"
#undef NEVERD_CONFIGURATION_ARM
#undef NEVERD_CONFIGURATION_X64
#undef NEVERD_CONFIGURATION_VALUE
#define NEVERD_RUN_CONTROL_TEST_VALUE(Name, Value)                             \
  constexpr unsigned Name = Value;
#include "RunControlCases.def"
#undef NEVERD_RUN_CONTROL_TEST_VALUE

struct HeldEntry {
  std::promise<void> Entered, Release;
  bool WaitForDeadline = false, SawStop = false;

  llvm::Error enter(MachineRunControl Control) {
    Entered.set_value();
    if (WaitForDeadline) {
      std::mutex Mutex;
      std::condition_variable Changed;
      std::unique_lock Lock(Mutex);
      Changed.wait_until(Lock, Control.Deadline, [] { return false; });
    } else {
      Release.get_future().wait();
    }
    SawStop = Control.stopRequested();
    return diagnostic::error(diagnostic::KvmRun);
  }
};
class HeldX64Machine final : public X64Machine {
public:
  explicit HeldX64Machine(HeldEntry &Entry) : Entry(Entry) {}
  llvm::Error step(X64MachineState &, uint64_t,
                   MachineRunControl Control) override {
    return Entry.enter(Control);
  }

private:
  HeldEntry &Entry;
};
class HeldAArch64Machine final : public AArch64Machine {
public:
  explicit HeldAArch64Machine(HeldEntry &Entry) : Entry(Entry) {}
  llvm::Error step(AArch64MachineState &, MachineRunControl Control) override {
    return Entry.enter(Control);
  }

private:
  HeldEntry &Entry;
};

class MachineControl : public ::testing::TestWithParam<GuestArchitecture> {
protected:
  HeldEntry Entry;
  std::unique_ptr<ExecutionBackend> CPU;

  void SetUp() override {
    auto Memory = llvm::cantFail(MemoryProjection::create(MemoryLimit));
    if (GetParam() == GuestArchitecture::X64) {
      CPU = llvm::cantFail(CheckedX64Backend::create(
          std::move(Memory), std::make_unique<HeldX64Machine>(Entry)));
    } else {
      CPU = llvm::cantFail(CheckedAArch64Backend::create(
          std::move(Memory), std::make_unique<HeldAArch64Machine>(Entry)));
    }
    llvm::cantFail(CPU->map(Code, PageSize, Read | Write | Execute));
    if (GetParam() == GuestArchitecture::X64) {
      llvm::cantFail(CPU->write(Code, LoopX64));
    } else {
      std::vector<uint8_t> Bytes(sizeof(LoopARM));
      llvm::support::endian::write32le(Bytes.data(), LoopARM[0]);
      llvm::cantFail(CPU->write(Code, Bytes));
    }
  }

  void checkTerminal(const ExecutionExit &Exit) {
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
    EXPECT_EQ(Exit.Diagnostic, diagnostic::KvmRun);
    ASSERT_TRUE(Exit.Fault);
    EXPECT_EQ(Exit.Fault->PC, Code);
    auto Resume = CPU->runUntilExit(Code, Timeout);
    ASSERT_FALSE(bool(Resume));
    llvm::consumeError(Resume.takeError());
  }
};

TEST_P(MachineControl, StopReachesActiveTransportAndFailureRemainsTerminal) {
  auto Entered = Entry.Entered.get_future();
  auto Run = std::async(std::launch::async,
                        [&] { return CPU->runUntilExit(Code, Timeout); });
  EXPECT_EQ(Entered.wait_for(std::chrono::milliseconds(WaitMilliseconds)),
            std::future_status::ready);
  CPU->stop();
  Entry.Release.set_value();
  const auto Exit = llvm::cantFail(Run.get());
  EXPECT_TRUE(Entry.SawStop);
  EXPECT_TRUE(Exit.StopRequested);
  EXPECT_FALSE(Exit.DeadlineReached);
  checkTerminal(Exit);
}

TEST_P(MachineControl, TransportFailurePreservesExpiredBudget) {
  Entry.WaitForDeadline = true;
  const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_FALSE(Entry.SawStop);
  EXPECT_FALSE(Exit.StopRequested);
  EXPECT_TRUE(Exit.DeadlineReached);
  EXPECT_TRUE(CPU->timedOut());
  checkTerminal(Exit);
}

INSTANTIATE_TEST_SUITE_P(Architectures, MachineControl,
                         ::testing::Values(GuestArchitecture::X64,
                                           GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
