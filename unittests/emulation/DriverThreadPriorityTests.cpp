//===- DriverThreadPriorityTests.cpp - Priority driver execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/kernel/KernelAPINames.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#define NEVERD_DRIVER_PRIORITY_CASE(Name, Value)                               \
  constexpr uint32_t Name = Value;
#define NEVERD_DRIVER_PRIORITY_VALUE(Name, Value)                              \
  constexpr uint64_t Name = Value;
#define NEVERD_DRIVER_PRIORITY_TEXT(Name, Value) constexpr char Name[] = Value;
#include "fixtures/DriverThreadPriorityCases.def"
#undef NEVERD_DRIVER_PRIORITY_TEXT
#undef NEVERD_DRIVER_PRIORITY_VALUE
#undef NEVERD_DRIVER_PRIORITY_CASE

struct PriorityParameter {
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  uint32_t Code;
  std::string Name;
};
std::vector<PriorityParameter> priorityParameters() {
  std::vector<PriorityParameter> Result;
  for (auto Backend : {ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                       ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? LegacySuffix
                                                 : CheckedSuffix);
#define NEVERD_DRIVER_PRIORITY_CASE(Name, Value)                               \
  Result.push_back({Backend, Contract, Value, Prefix + #Name});
#include "fixtures/DriverThreadPriorityCases.def"
#undef NEVERD_DRIVER_PRIORITY_CASE
    }
  return Result;
}

class DriverSchedulingPriority
    : public testing::TestWithParam<PriorityParameter> {};

TEST_P(DriverSchedulingPriority, RuntimePriorityControlsDispatchAndPreemption) {
  const auto &P = GetParam();
  auto Probe = createExecutionBackend(P.Backend, P.Contract, ProbeMemory);
  if (!Probe) {
    auto Error = Probe.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  DriverOptions Options;
  Options.Backend = P.Backend;
  Options.Contract = P.Contract;
  Options.ReportBackendSelection = true;
  Options.Scheduling = DriverScheduling{
      P.Code == EqualRoundRobin ? SmallQuantum : LargeQuantum, InstructionTime};
  Options.InstructionLimit = InstructionBudget;
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Options.Requests.push_back(Create);
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = P.Code;
  IO.OutputSize =
      P.Code == QuantumRemainder ? RemainderOutputLength : OutputLength;
  Options.Requests.push_back(IO);
  for (auto Kind : {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    Options.Requests.push_back(Request);
  }
  Options.Unload = true;
  const auto Run = [&] {
    return emulateDriver(std::string(NEVERD_DRIVER_FIXTURES) + Image, Options);
  };
  const auto Check = [&](const DriverResult &Result) {
    ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
    EXPECT_EQ(Result.SelectedBackend, P.Backend);
    ASSERT_EQ(Result.Requests.size(), Options.Requests.size());
    ASSERT_EQ(Result.Requests[1].Output.size(), Options.Requests[1].OutputSize);
    EXPECT_EQ(Result.Requests[1].Output[0], SuccessMarker);
    EXPECT_TRUE(std::any_of(
        Result.Calls.begin(), Result.Calls.end(), [](const auto &Call) {
          return Call.Name == kernel_api::KeSetPriorityThread;
        }));
    EXPECT_TRUE(std::any_of(
        Result.Calls.begin(), Result.Calls.end(), [](const auto &Call) {
          return Call.Name == kernel_api::KeQueryPriorityThread;
        }));
  };
  auto Result = Run();
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  Check(*Result);
  auto Again = Run();
  ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
  Check(*Again);
  EXPECT_EQ(Again->Instructions, Result->Instructions);
  ASSERT_EQ(Again->Calls.size(), Result->Calls.size());
  for (size_t I = 0; I < Result->Calls.size(); ++I) {
    EXPECT_EQ(Again->Calls[I].Name, Result->Calls[I].Name);
    EXPECT_EQ(Again->Calls[I].Phase, Result->Calls[I].Phase);
    EXPECT_EQ(Again->Calls[I].Result, Result->Calls[I].Result);
  }
  if (P.Code == QuantumRemainder) {
    // These executions run the identical counter loop and equal-priority peer.
    // Only one has a higher-priority timer interruption inside the first slice.
    // The peer must see exactly the same counter, proving the saved remainder
    // was consumed rather than replaced with a fresh quantum on resumption.
    Options.Requests[1].ControlCode = RemainderBaselineCode;
    auto Baseline = Run();
    ASSERT_TRUE(bool(Baseline)) << llvm::toString(Baseline.takeError());
    Check(*Baseline);
    ASSERT_EQ(Result->Requests.size(), Options.Requests.size());
    ASSERT_EQ(Result->Requests[1].Output.size(), RemainderOutputLength);
    ASSERT_EQ(Baseline->Requests.size(), Options.Requests.size());
    ASSERT_EQ(Baseline->Requests[1].Output.size(), RemainderOutputLength);
    const auto Counter = [](const DriverResult &Run) {
      return llvm::support::endian::read32le(Run.Requests[1].Output.data() +
                                             OutputLength);
    };
    EXPECT_EQ(Counter(*Result), Counter(*Baseline));
    EXPECT_GT(Counter(*Result), 0u);
  }
  if (P.Code == EqualRoundRobin)
    for (uint64_t Quantum : {uint64_t(1), SmallQuantum + 1}) {
      Options.Scheduling->QuantumInstructions = Quantum;
      auto Tiny = Run();
      ASSERT_TRUE(bool(Tiny)) << llvm::toString(Tiny.takeError());
      Check(*Tiny);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Preemptive, DriverSchedulingPriority,
    testing::ValuesIn(priorityParameters()),
    [](const testing::TestParamInfo<PriorityParameter> &P) {
      return P.param.Name;
    });
} // namespace
} // namespace neverd::emulation
