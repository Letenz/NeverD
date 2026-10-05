//===- DriverSIMDSEHTests.cpp - Native SSE faults through original WDK C --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "fixtures/driver_seh_test.h"
#include "gtest/gtest.h"
#include "os/windows/exception/X64SEH.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace {
struct SIMDParameter {
  ExecutionBackendKind Backend;
  ExecutionContract Contract;
  char Mode;
  std::string Name;
};
std::vector<SIMDParameter> parameters() {
  std::vector<SIMDParameter> Result;
  for (auto Backend : {ExecutionBackendKind::KVM, ExecutionBackendKind::WHP})
    for (auto Contract :
         {ExecutionContract::Legacy, ExecutionContract::CheckedX64}) {
      const std::string Prefix =
          std::string(executionBackendName(Backend)) +
          (Contract == ExecutionContract::Legacy ? SehSIMDDriverSuffix
                                                 : SehSIMDCheckedSuffix);
#define NEVERD_SEH_SIMD_MODE(Name, Value)                                      \
  Result.push_back({Backend, Contract, Value, Prefix + #Name});
#include "fixtures/driver_seh_simd.def"
#undef NEVERD_SEH_SIMD_MODE
    }
  return Result;
}
class DriverSIMDSEH : public testing::TestWithParam<SIMDParameter> {};

TEST_P(DriverSIMDSEH, OriginalDriverCatchesAndRetriesSSEFaults) {
#ifdef NEVERD_WDM_SEH_FIXTURE
  const auto &Parameter = GetParam();
  auto CPU = createExecutionBackend(Parameter.Backend, Parameter.Contract,
                                    profile::DefaultMemoryLimit);
  if (!CPU) {
    auto Error = CPU.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    const auto Text = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Text;
    FAIL() << Text;
  }
  ASSERT_TRUE(CPU->CPU->supportsSIMDExceptions());
  std::vector<const char *> Images{NEVERD_WDM_SEH_FIXTURE};
#ifdef NEVERD_WDM_SEH_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_SEH_CFG_FIXTURE);
#endif
  constexpr size_t Cases = 0
#define NEVERD_SEH_SIMD_CASE(Name, ...) +1
#include "fixtures/driver_seh_simd.def"
#undef NEVERD_SEH_SIMD_CASE
      ;
  for (const auto *Image : Images)
    for (uint64_t Base : {uint64_t(SehSIMDBase), uint64_t(SehSIMDRebased)}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Base);
      DriverOptions Options;
      Options.Backend = Parameter.Backend;
      Options.Contract = Parameter.Contract;
      Options.LoadAddress = Base;
      Options.Unload = true;
      Options.ServiceName =
          std::string(SehSIMDService) + char(SehSIMDMarker) + Parameter.Mode;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      if (Parameter.Mode == SehSIMDRejectX87) {
        EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
        EXPECT_NE(Result->Diagnostic.find(
                      seh::text::
                          ContinuationChangesUnsupportedExceptionContextFields),
                  std::string::npos)
            << Result->Diagnostic;
        EXPECT_FALSE(Result->UnloadCompleted);
        continue;
      }
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      EXPECT_EQ(Result->NTStatus, 0u);
      EXPECT_EQ(Result->ImageBase, Base);
      EXPECT_FALSE(Result->Fault);
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_TRUE(Result->Requests.empty());
      EXPECT_TRUE(Result->Devices.empty());
      for (const auto &Message : Result->Messages)
        EXPECT_EQ(Message.find(SehSIMDFailure), std::string::npos) << Message;
      for (const auto *Format : {SehSIMDFilterMessage, SehSIMDFinallyMessage,
                                 SehSIMDCompleteMessage}) {
        const llvm::StringRef Text(Format);
        const auto Prefix = Text.take_front(Text.find('%'));
        const size_t Expected = Parameter.Mode == SehSIMDConstant &&
                                        Format != SehSIMDCompleteMessage
                                    ? 0
                                    : Cases;
        EXPECT_EQ(
            std::count_if(Result->Messages.begin(), Result->Messages.end(),
                          [&](const auto &Message) {
                            return llvm::StringRef(Message).starts_with(Prefix);
                          }),
            Expected);
      }
    }
#else
  GTEST_SKIP() << SehSIMDMissing;
#endif
}
INSTANTIATE_TEST_SUITE_P(Native, DriverSIMDSEH, testing::ValuesIn(parameters()),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
