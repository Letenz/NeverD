//===- X64ProbeExecutionTests.cpp - Original native startup execution ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64MachineProbe.h"
#include "backends/MachineFactories.h"
#include "core/ExecutionDiagnostics.h"
#include "core/MemoryProjection.h"
#include "gtest/gtest.h"

#include <cstring>

namespace neverd::emulation {
namespace {
#define NEVERD_X64_PROBE_TEST_VALUE(Name, Value)                               \
  constexpr uint64_t Name = Value;
#define NEVERD_X64_PROBE_TEST_CODE(Name, ...)                                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64ProbeCases.def"
#undef NEVERD_X64_PROBE_TEST_CODE
#undef NEVERD_X64_PROBE_TEST_VALUE
class OriginalTransport final : public X64Machine {
public:
  X64Machine &Native;
  MemoryProjection &Memory;
  unsigned Entries = 0;
  std::optional<std::chrono::steady_clock::time_point> Deadline;
  OriginalTransport(X64Machine &Native, MemoryProjection &Memory)
      : Native(Native), Memory(Memory) {}
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    if (!Entries) {
      Deadline = Control.Deadline;
      auto *Code =
          Memory.data() + x64::gateway::CodeGPA + x64::probe::CodeOffset;
      EXPECT_EQ(llvm::ArrayRef<uint8_t>(Code, sizeof(Program)),
                llvm::ArrayRef<uint8_t>(Program));
      // The independent assembler bytes, not host-side projected results,
      // execute on the actual CPU inside the private supervisor mapping.
      std::memcpy(Code, Program, sizeof(Program));
    }
    EXPECT_EQ(Control.Deadline, *Deadline);
    EXPECT_EQ(Control.Stop, nullptr);
    ++Entries;
    return Native.step(State, Root, Control);
  }
};
struct ProbeBackend {
  ExecutionBackendKind Backend;
};
void PrintTo(const ProbeBackend &Value, std::ostream *OS) {
  *OS << executionBackendName(Value.Backend);
}
class X64ProbeExecution : public testing::TestWithParam<ProbeBackend> {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  std::unique_ptr<X64Machine> Machine;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    auto Created = GetParam().Backend == ExecutionBackendKind::KVM
                       ? createKvmMachine(*Memory)
                       : createWhpMachine(*Memory);
    if (!Created) {
      auto E = Created.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Machine = std::move(*Created);
  }
};
TEST_P(X64ProbeExecution, OriginalProgramChecksFullStateAndOneDeadline) {
  static_assert(sizeof(Program) == x64::probe::programBytes());
  OriginalTransport Original(*Machine, *Memory);
  ASSERT_EQ(llvm::toString(verifyX64Machine(Original, *Memory)), "");
  EXPECT_EQ(Original.Entries, Instructions);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), "");
}
TEST_P(X64ProbeExecution, ExistingGuestMappingAndRAMSurviveInitialization) {
  std::vector<uint8_t> Before(x64::gateway::Bytes, Fill);
  llvm::cantFail(Memory->map(x64::KernelMin, Before.size(), Read | Write));
  llvm::cantFail(Memory->write(x64::KernelMin, Before));
  OriginalTransport Original(*Machine, *Memory);
  ASSERT_EQ(llvm::toString(verifyX64Machine(Original, *Memory)), "");
  EXPECT_GE(x64ExceptionMonitorBase(*Memory), x64::KernelMin + Before.size());
  std::vector<uint8_t> After(Before.size());
  ASSERT_EQ(llvm::toString(Memory->read(x64::KernelMin, After)), "");
  EXPECT_EQ(After, Before);
  EXPECT_EQ(Original.Entries, Instructions);
}
INSTANTIATE_TEST_SUITE_P(
    Native, X64ProbeExecution,
    testing::Values(ProbeBackend{ExecutionBackendKind::KVM},
                    ProbeBackend{ExecutionBackendKind::WHP}),
    [](const auto &Info) { return executionBackendName(Info.param.Backend); });
} // namespace
} // namespace neverd::emulation
