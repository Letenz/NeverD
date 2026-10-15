//===- DriverBugCheckTests.cpp - Original callback lifecycle execution ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/Endian.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
class DriverBugCheckExecution
    : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  DriverOptions Options;
  std::filesystem::path Path =
      std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_bugcheck.sys";
  void SetUp() override {
    Options.Backend = GetParam();
    Options.Contract = ExecutionContract::CheckedX64;
    auto Probe = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(Path));
    Options.Unload = true;
    for (auto Kind :
         {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
          DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
      DriverRequest Request;
      Request.Kind = Kind;
      if (Kind == DriverRequestKind::DeviceControl) {
        Request.ControlCode = 0x222000;
        Request.OutputSize = 4;
      }
      Options.Requests.push_back(Request);
    }
  }
};

TEST_P(DriverBugCheckExecution,
       RegistrationSurvivesRequestsAndRetiresInUnload) {
  for (uint64_t Base : {uint64_t(0), uint64_t(0x1a0000000)}) {
    Options.LoadAddress = Base;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    EXPECT_TRUE(Run->UnloadCompleted);
    EXPECT_TRUE(Run->Devices.empty());
    ASSERT_EQ(Run->Requests.size(), 4u);
    for (const auto &Request : Run->Requests) {
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.IOStatus, 0u);
    }
    ASSERT_EQ(Run->Requests[1].Output.size(), 4u);
    EXPECT_EQ(llvm::support::endian::read32le(Run->Requests[1].Output.data()),
              6u);
  }
}

TEST_P(DriverBugCheckExecution, OpaqueAccessAndLeakedRegistrationRefuse) {
  for (auto [Code, Text] : {std::pair{0x222004u, "opaque bug-check"},
                            {0x222008u, "read-only bug-check"},
                            {0x222010u, "opaque bug-check"},
                            {0x22200cu, "live bug-check"}}) {
    Options.Requests[1].ControlCode = Code;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
    EXPECT_NE(Run->Diagnostic.find(Text), std::string::npos) << Run->Diagnostic;
    EXPECT_FALSE(Run->UnloadCompleted);
  }
}

TEST_P(DriverBugCheckExecution, RejectedStorePreservesRegisteredRecord) {
  auto Backend = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  constexpr uint64_t Base = 0x180000000, Record = Base + 4096;
  llvm::cantFail(CPU.map(Base, 8192, Read | Write | Execute));
  DriverImage Image;
  Image.Base = Image.Entry = Base;
  Image.Size = 8192;
  Image.Regions.push_back(
      {Base, Read | Write | Execute, std::vector<uint8_t>(8192)});
  DriverResult Result;
  KernelModel Model(CPU, Result);
  llvm::cantFail(Model.initialize(Image, Options));
  auto Registered = Model.call("KeRegisterBugCheckReasonCallback",
                               {Record, Base, 2, Record + 48});
  ASSERT_TRUE(bool(Registered)) << llvm::toString(Registered.takeError());
  ASSERT_EQ(*Registered, 1u);
  std::array<uint8_t, 48> Before{}, After{};
  llvm::cantFail(CPU.snapshotBacking(Record, Before));
  // mov byte ptr [rcx+44],0; ud2. Refusal must precede the actual store.
  const uint8_t Body[] = {0xc6, 0x41, 0x2c, 0, 0x0f, 0x0b};
  llvm::cantFail(CPU.write(Base, Body));
  llvm::cantFail(CPU.setReg(X64Register::CX, Record));
  std::string Diagnostic;
  BackendHooks Hooks;
  Hooks.Write = [&](uint64_t Address, uint32_t Size, uint64_t) {
    if (auto E = Model.validateGuestAccess(Address, Size, true)) {
      Diagnostic = llvm::toString(std::move(E));
      CPU.stop();
    }
  };
  llvm::cantFail(CPU.installHooks(std::move(Hooks)));
  auto Exit = CPU.runUntilExit(Base, 1000000);
  ASSERT_TRUE(bool(Exit)) << llvm::toString(Exit.takeError());
  EXPECT_NE(Diagnostic.find("read-only bug-check"), std::string::npos);
  llvm::cantFail(CPU.snapshotBacking(Record, After));
  EXPECT_EQ(After, Before);
}

INSTANTIATE_TEST_SUITE_P(
    Native, DriverBugCheckExecution,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP),
    [](const testing::TestParamInfo<ExecutionBackendKind> &P) {
      return std::string(executionBackendName(P.param));
    });
} // namespace
} // namespace neverd::emulation
