//===- AArch64ProbeExecutionTests.cpp - ARM64 probe execution --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <iterator>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_PROBE_TEST_VALUE(Name, Value)                           \
  constexpr uint64_t Name = Value;
#define NEVERD_AARCH64_PROBE_TEST_CODE(Name, ...)                              \
  constexpr uint32_t Name[] = {__VA_ARGS__};
#include "AArch64ProbeCases.def"
#undef NEVERD_AARCH64_PROBE_TEST_CODE
#undef NEVERD_AARCH64_PROBE_TEST_VALUE

/// Relocate the private, PC-independent startup words to ordinary guest code.
/// This exercises the real transports at both privileges without granting user
/// access to monitor pages. It is not a native startup or privilege probe.
class RelocatedProbe final : public AArch64Machine {
public:
  AArch64Machine &Machine;
  MemoryProjection &Memory;
  bool UserMode;
  std::vector<std::chrono::steady_clock::time_point> Deadlines;
  std::vector<uint32_t> Instructions;
  RelocatedProbe(AArch64Machine &Machine, MemoryProjection &Memory,
                 bool UserMode)
      : Machine(Machine), Memory(Memory), UserMode(UserMode) {}
  llvm::Error step(AArch64MachineState &State,
                   MachineRunControl Control) override {
    Deadlines.push_back(Control.Deadline);
    const uint64_t OriginalPC = State.reg(AArch64Register::PC);
    Instructions.push_back(
        llvm::support::endian::read32le(Memory.data() + OriginalPC));
    // Initialization owns the physical lease. Borrow it for projection only;
    // original guest words were installed before that lease was acquired.
    auto Lease = Memory.executionLock();
    if (!Lease)
      return Lease.takeError();
    if (auto E = buildAArch64PageTables(Memory, UserMode))
      return E;
    auto Next = State;
    Next.UserMode = UserMode;
    const uint64_t GuestPC =
        Code + (Instructions.size() - 1) * aarch64::InstructionBytes;
    Next.reg(AArch64Register::PC) = GuestPC;
    if (auto E = Machine.step(Next, Control))
      return E;
    if (Next.UserMode != UserMode ||
        Next.reg(AArch64Register::PC) != GuestPC + aarch64::InstructionBytes)
      return diagnostic::error(diagnostic::ArmState);
    Next.UserMode = State.UserMode;
    Next.reg(AArch64Register::PC) = OriginalPC + aarch64::InstructionBytes;
    State = Next;
    return llvm::Error::success();
  }
};
using Parameter = std::tuple<ExecutionBackendKind, bool>;
class AArch64ProbeExecution : public testing::TestWithParam<Parameter> {};
TEST_P(AArch64ProbeExecution,
       OriginalProgramChecksCompleteStateAndOneDeadline) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  const auto Backend = std::get<0>(GetParam());
  const bool UserMode = std::get<1>(GetParam());
  auto Created = Backend == ExecutionBackendKind::KVM
                     ? createKvmAArch64Machine(*Memory)
                 : Backend == ExecutionBackendKind::WHP
                     ? createWhpAArch64Machine(*Memory)
                     : createUnicornAArch64Machine(*Memory, UserMode);
  if (!Created) {
    auto E = Created.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Text = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  auto Machine = std::move(*Created);
  llvm::cantFail(Memory->map(Code, memory::PageSize,
                             Read | Write | Execute | UserAccessible));
  std::vector<uint8_t> Bytes(sizeof(Program));
  for (unsigned Index = 0; Index < std::size(Program); ++Index)
    llvm::support::endian::write32le(
        Bytes.data() + Index * aarch64::InstructionBytes, Program[Index]);
  llvm::cantFail(Memory->write(Code, Bytes));
  RelocatedProbe Probe(*Machine, *Memory, UserMode);
  ASSERT_EQ(llvm::toString(verifyAArch64Machine(Probe, *Memory)), "");
  EXPECT_EQ(Probe.Instructions,
            std::vector<uint32_t>(std::begin(Program), std::end(Program)));
  ASSERT_EQ(Probe.Deadlines.size(), ProbeInstructions);
  for (const auto &Deadline : Probe.Deadlines)
    EXPECT_EQ(Deadline, Probe.Deadlines.front());
}
INSTANTIATE_TEST_SUITE_P(
    Transports, AArch64ProbeExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Bool()));
} // namespace
} // namespace neverd::emulation
