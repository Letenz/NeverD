//===- AndroidObservationTests.cpp - Unmodified execution observations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare complete workloads with and without read-only execution watches.
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

#include <set>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x21000000, Code = 0x22000000;
class AndroidObservation : public testing::TestWithParam<
                               std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    auto &A = *Options.Android;
    A.Initialize = false;
    A.EntrySymbol = "observation_call";
    A.Arguments = {Code, Buffer, 0};
    A.TraceLimit = 4096;
    std::vector<uint8_t> Data(64, 0xc7);
    llvm::support::endian::write64le(Data.data() + 16, 42);
    llvm::support::endian::write64le(Data.data() + 48, 84);
    A.Memory.push_back({Buffer, 4096, Data, false});
    // ldr x1,[x0,#16]; str x1,[x0]; add x1,x1,#1;
    // str x1,[x0,#8]; mov x0,x1; ret
    const uint32_t Instructions[] = {0xf9400801, 0xf9000001, 0x91000421,
                                     0xf9000401, 0xaa0103e0, 0xd65f03c0};
    std::vector<uint8_t> Bytes(sizeof(Instructions));
    for (unsigned I = 0; I < 6; ++I)
      llvm::support::endian::write32le(Bytes.data() + 4 * I, Instructions[I]);
    A.Memory.push_back({Code, 4096, Bytes, true});
    A.ReadMemory = {{Buffer, 64}, {Code, sizeof(Instructions)}};
    Options.LinuxTime.emplace();
    Options.LinuxTime->Clocks[1] = {3, 0};
    Options.LinuxTime->NanosecondsPerInstruction = 7;
#endif
  }
  void observe(uint64_t Limit = 6) {
    Options.Android->ObserveExecution = NativeExecutionObservationOptions{
        {Code, Code + 4, Code + 8, Code + 12, Code + 16, Code + 20},
        Limit,
        {{Buffer, 64}, {Code, 24}}};
  }
  ProcessResult run() {
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void sameWorkload(const ProcessResult &Before, ProcessResult After) {
    After.ExecutionObservationsEnabled = false;
    EXPECT_EQ(processResultJSON(Before), processResultJSON(After));
  }
};
TEST_P(AndroidObservation, ExactBeforeInstructionStateAndUnchangedWorkload) {
  for (uint64_t Quantum : {1, 2, 3, 1000}) {
    SCOPED_TRACE(Quantum);
    Options.InstructionQuantum = Quantum;
    Options.Android->ObserveExecution.reset();
    const auto Baseline = run();
    ASSERT_EQ(Baseline.Stop, ProcessStopReason::Returned);
    ASSERT_EQ(Baseline.ReturnValue, 43u);
    observe();
    auto R = run();
    ASSERT_EQ(R.ExecutionObservations.size(), 6u);
    EXPECT_FALSE(R.ExecutionObservationsTruncated);
    sameWorkload(Baseline, R);
    for (unsigned I = 0; I < 6; ++I) {
      const auto &O = R.ExecutionObservations[I];
      EXPECT_EQ(O.PC, Code + I * 4);
      EXPECT_EQ(O.Registers[0], I < 5 ? Buffer : 43);
      EXPECT_EQ(O.Registers[1], I == 0 ? 0 : I < 3 ? 42 : 43);
      EXPECT_EQ(O.SP % 16, 0u);
      EXPECT_EQ(O.NZCV & ~uint64_t(0xf0000000), 0u);
      EXPECT_EQ(O.ThreadID, 1000u);
      ASSERT_EQ(O.Memory.size(), 2u);
      auto Expected = Options.Android->Memory[0].Bytes;
      if (I >= 2)
        llvm::support::endian::write64le(Expected.data(), 42);
      if (I >= 4)
        llvm::support::endian::write64le(Expected.data() + 8, 43);
      EXPECT_EQ(O.Memory[0].Bytes, Expected);
      EXPECT_EQ(O.Memory[1].Bytes, Options.Android->Memory[1].Bytes);
      EXPECT_EQ(R.Trace[O.Instructions], O.PC);
    }
    auto JSON = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
    const auto *A = JSON.getAsObject()->getObject("android");
    ASSERT_NE(A, nullptr);
    ASSERT_EQ(A->getArray("execution_observations")->size(), 6u);
    EXPECT_EQ(A->getBoolean("execution_observations_truncated"), false);
  }
}
TEST_P(AndroidObservation, TruncationRequiresAnAdditionalMatch) {
  auto Baseline = run();
  for (uint64_t Limit : {1, 5, 6, 7}) {
    observe(Limit);
    auto R = run();
    EXPECT_EQ(R.ExecutionObservations.size(), std::min(Limit, uint64_t(6)));
    EXPECT_EQ(R.ExecutionObservationsTruncated, Limit < 6);
    sameWorkload(Baseline, R);
  }
}
TEST_P(AndroidObservation, ThreadIdentityAndQuantumOrderArePreserved) {
  Options.Android->EntrySymbol = "observation_threads";
  Options.Android->Arguments = {Code, Buffer};
  Options.Android->ThreadLimit = 3;
  for (uint64_t Quantum : {1, 2, 3, 7}) {
    SCOPED_TRACE(Quantum);
    Options.InstructionQuantum = Quantum;
    Options.Android->ObserveExecution.reset();
    auto Baseline = run();
    ASSERT_EQ(Baseline.ReturnValue, 126u) << Baseline.Diagnostic;
    observe(12);
    auto R = run();
    ASSERT_EQ(R.ExecutionObservations.size(), 12u);
    EXPECT_FALSE(R.ExecutionObservationsTruncated);
    sameWorkload(Baseline, R);
    std::map<uint64_t, std::vector<uint64_t>> PCs;
    for (const auto &O : R.ExecutionObservations) {
      PCs[O.ThreadID].push_back(O.PC);
      EXPECT_EQ(R.Trace[O.Instructions], O.PC);
    }
    ASSERT_EQ(PCs.size(), 2u);
    for (const auto &[ID, Sequence] : PCs) {
      EXPECT_NE(ID, 1000u);
      EXPECT_EQ(Sequence, Options.Android->ObserveExecution->PCs);
    }
  }
}
TEST_P(AndroidObservation, UnknownServiceIsNotSkippedOrGivenAReturn) {
  Options.Android->EntrySymbol = "unknown_call";
  Options.Android->Arguments.clear();
  auto Baseline = run();
  ASSERT_EQ(Baseline.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(Baseline.NativeCalls.empty());
  const uint64_t PC = Baseline.NativeCalls.back().PC;
  observe();
  Options.Android->ObserveExecution->PCs = {PC};
  auto R = run();
  ASSERT_EQ(R.ExecutionObservations.size(), 1u);
  EXPECT_EQ(R.ExecutionObservations[0].Registers[0], 19u);
  EXPECT_FALSE(R.NativeCalls.back().Result);
  sameWorkload(Baseline, R);
}
TEST_P(AndroidObservation, ExplicitMemoryMustBeReadableAtItsObservation) {
  observe();
  auto &Reads = Options.Android->ObserveExecution->ReadMemory;
  Reads = {{0x23000000, 8, false}};
  auto R = run();
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_EQ(R.PC, Code);
  EXPECT_TRUE(R.ExecutionObservations.empty());
  EXPECT_EQ(R.Diagnostic.find("observation memory is not readable") !=
                std::string::npos,
            true);
  Reads[0].RequireMappedAtEntry = true;
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
}
TEST_P(AndroidObservation, WholeObservationBudgetIsReservedBeforeExecution) {
  observe();
  auto &O = *Options.Android->ObserveExecution;
  O.Limit = UINT64_MAX;
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  O.Limit = 1;
  O.ReadMemory = {{0, Options.OutputLimit}};
  R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}
TEST(AndroidObservationOptions, StrictShapeAndLosslessAddresses) {
  auto Parse = [](const char *Value) {
    return processOptionsFromJSON(
        std::string("{\"android\":{\"observe_execution\":") + Value + "}}");
  };
  auto Good = Parse(
      R"({"pcs":["0xfffffffffffffffc",4096],"limit":3,"read_memory":[{"address":"0x21000000","size":8,"require_mapped_at_entry":false}]})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->Android->ObserveExecution->PCs[0], UINT64_MAX - 3);
  EXPECT_FALSE(
      Good->Android->ObserveExecution->ReadMemory[0].RequireMappedAtEntry);
  for (
      const char *Bad :
      {"null", "[]", "{}", R"({"pcs":[],"limit":1})",
       R"({"pcs":[4096],"limit":0})", R"({"pcs":[4097],"limit":1})",
       R"({"pcs":[4096,4096],"limit":1})", R"({"pcs":[true],"limit":1})",
       R"({"pcs":[4096],"limit":1,"extra":0})",
       R"({"pcs":[4096],"limit":1,"read_memory":[{"address":1,"size":0}]})",
       R"({"pcs":[4096],"limit":1,"read_memory":[{"address":"0xffffffffffffffff","size":2}]})",
       R"({"pcs":[4096],"limit":1,"read_memory":[{"address":1,"size":8,"bytes_hex":"00"}]})"}) {
    auto R = Parse(Bad);
    EXPECT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
}
INSTANTIATE_TEST_SUITE_P(
    Relocations, AndroidObservation,
    testing::Combine(testing::Values("none", "android", "relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF)));
} // namespace
} // namespace neverd::emulation
