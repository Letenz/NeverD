//===- DriverFileSystemExecutionTests.cpp - Declared volume execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise original and genuine WDK volume identity and failure boundaries.

#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
DriverFileSystemVolume declaredVolume() {
  DriverFileSystemNode Root;
  Root.Path = "\\";
  Root.Kind = DriverFileNodeKind::Directory;
  Root.FileID = 0;
  Root.Attributes = 0x10;
  Root.AllocationSize = 0;
  Root.CreationTime100ns = Root.LastAccessTime100ns = Root.LastWriteTime100ns =
      Root.ChangeTime100ns = 0;
  auto File = Root;
  File.Path = "\\Data";
  File.Kind = DriverFileNodeKind::File;
  File.FileID = UINT64_MAX;
  File.Attributes = 0x20;
  File.AllocationSize = 4096;
  File.CreationTime100ns = 1;
  File.LastAccessTime100ns = 2;
  File.LastWriteTime100ns = 3;
  File.ChangeTime100ns = 4;
  File.Data = std::vector<uint8_t>{0, 0x7f, 0x80, 0xff};
  DriverFileSystemVolume Volume;
  Volume.DeviceName = "\\Device\\DeclaredVolume";
  Volume.AccessPolicy = DriverFileAccessPolicy::FullAccess;
  Volume.SectorSize = 4096;
  Volume.AlignmentMask = 511;
  Volume.Nodes = {Root, File};
  return Volume;
}
class KernelFileSystemExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {
protected:
  void runOpens(const std::filesystem::path &Image) {
    const auto [Backend, Contract, Rebase] = GetParam();
    auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Probe->CPU.reset();
    for (bool Scheduled : {false, true})
      for (char Mode : {'C', 'P', 'X', 'U', 'R', 'Q', 'B', 'V', 'K', 'T'}) {
        SCOPED_TRACE(::testing::Message()
                     << Image << ' ' << Scheduled << ' ' << Mode);
        DriverOptions Options;
        Options.Backend = Backend;
        Options.Contract = Contract;
        Options.Unload = true;
        Options.ServiceName = std::string("FileOpen") + Mode;
        Options.FileSystem = std::vector{declaredVolume()};
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->NTStatus, 0u) << Result->Diagnostic;
        ASSERT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
        EXPECT_TRUE(Result->Devices.empty());
        ASSERT_FALSE(Result->Requests.empty());
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_TRUE(Request.FileObject);
          EXPECT_TRUE(Request.FileOpenOperation);
          EXPECT_TRUE(Request.FileSystemDevice);
          EXPECT_TRUE(Request.FileSystemFileID);
        }
        if (Mode == 'B' || Mode == 'V' || Mode == 'K' || Mode == 'T') {
          ASSERT_EQ(Result->Requests.size(), 3u);
          EXPECT_EQ(Result->Requests[0].Kind, DriverRequestKind::Create);
          EXPECT_EQ(Result->Requests[1].Kind, DriverRequestKind::Cleanup);
          EXPECT_EQ(Result->Requests[2].Kind, DriverRequestKind::Close);
          EXPECT_EQ(Result->Requests[0].Origin,
                    DriverRequestOrigin::FileCreate);
          EXPECT_EQ(Result->Requests[1].Origin,
                    DriverRequestOrigin::FileHandleClose);
          EXPECT_EQ(Result->Requests[2].Origin,
                    DriverRequestOrigin::FileFinalClose);
          unsigned Opens = 0, Lookups = 0;
          for (const auto &Call : Result->Calls) {
            if (Call.Name == "IoGetDeviceObjectPointer") {
              ++Opens;
              EXPECT_EQ(Call.Result, std::optional<uint64_t>{0});
            }
            if (Call.Name == "MmGetSystemRoutineAddress")
              ++Lookups;
          }
          EXPECT_EQ(Opens, 1u);
          EXPECT_EQ(Lookups, Mode == 'T' ? 1u : 0u);
        }
        auto JSON = llvm::json::parse(driverResultJSON(*Result));
        ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
        const auto *Requests = JSON->getAsObject()->getArray("requests");
        ASSERT_NE(Requests, nullptr);
        const auto *First = Requests->front().getAsObject();
        EXPECT_EQ(First->get("file")->kind(), llvm::json::Value::Null);
        EXPECT_TRUE(First->getString("file_object"));
        EXPECT_TRUE(First->getInteger("file_open_operation"));
        EXPECT_TRUE(First->getString("filesystem_device"));
        EXPECT_TRUE(First->get("filesystem_file_id"));
      }
  }
  void run(const std::filesystem::path &Image) {
    const auto [Backend, Contract, Rebase] = GetParam();
    auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    Probe->CPU.reset();
    for (bool Scheduled : {false, true})
      for (char Mode : {'S', 'L', 'G', 'F', 'W', 'N', 'O'}) {
        SCOPED_TRACE(::testing::Message()
                     << Image << ' ' << Scheduled << ' ' << Mode);
        DriverOptions Options;
        Options.Backend = Backend;
        Options.Contract = Contract;
        Options.Unload = true;
        Options.ServiceName = std::string("DeclaredFileSystem") + Mode;
        Options.FileSystem = std::vector{declaredVolume()};
        if (Mode == 'N')
          Options.FileSystem->clear();
        if (Mode == 'O')
          Options.FileSystem.reset();
        if (Mode == 'F') {
          DriverRequest Create;
          Create.Kind = DriverRequestKind::Create;
          Create.Device = "\\Device\\DeclaredVolume";
          Create.File = 1;
          Options.Requests = {Create};
        }
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        const bool Success = Mode == 'S' || Mode == 'N' || Mode == 'O';
        EXPECT_EQ(Result->Stop, Success ? DriverStopReason::Returned
                                        : DriverStopReason::ModelError)
            << Result->Diagnostic;
        EXPECT_EQ(Result->UnloadCompleted, Success) << Result->Diagnostic;
        if (Mode != 'W')
          EXPECT_EQ(Result->NTStatus, 0u);
        if (Mode == 'L')
          EXPECT_NE(Result->Diagnostic.find("references"), std::string::npos);
        if (Mode == 'G')
          EXPECT_NE(Result->Diagnostic.find("live devices"), std::string::npos);
        if (Mode == 'W')
          EXPECT_NE(Result->Diagnostic.find("opaque"), std::string::npos);
        if (Mode == 'F') {
          EXPECT_NE(Result->Diagnostic.find("not implemented"),
                    std::string::npos);
          ASSERT_EQ(Result->Requests.size(), 1u);
          EXPECT_EQ(Result->Requests.front().IRP, 0u);
        }
        if (Mode == 'O') {
          EXPECT_FALSE(Result->FileSystem);
          continue;
        }
        ASSERT_TRUE(Result->FileSystem);
        if (Mode == 'N') {
          EXPECT_TRUE(Result->FileSystem->empty());
          continue;
        }
        ASSERT_EQ(Result->FileSystem->size(), 1u);
        const auto &State = Result->FileSystem->front();
        EXPECT_NE(State.DeviceObject, 0u);
        ASSERT_EQ(State.Volume.Nodes.size(), 2u);
        const auto &File = State.Volume.Nodes[1];
        EXPECT_EQ(File.Data, Options.FileSystem->front().Nodes[1].Data);
        EXPECT_EQ(File.FileID, UINT64_MAX);
        EXPECT_EQ(File.AllocationSize, 4096u);
        EXPECT_EQ(File.ChangeTime100ns, 4u);
        // Legacy Unicorn can commit a rejected write before it stops. Device
        // projection failure must preserve the independent filesystem owner.
        if (Mode != 'W')
          EXPECT_EQ(Result->Devices.size(),
                    Mode == 'G' || Mode == 'F' ? 1u : 0u);
        auto JSON = llvm::json::parse(driverResultJSON(*Result));
        ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
        const auto *Volumes = JSON->getAsObject()->getArray("filesystem");
        ASSERT_NE(Volumes, nullptr);
        const auto *Nodes = Volumes->front().getAsObject()->getArray("nodes");
        ASSERT_NE(Nodes, nullptr);
        EXPECT_EQ((*Nodes)[1].getAsObject()->getInteger("end_of_file"), 4);
        EXPECT_EQ((*Nodes)[1].getAsObject()->getString("data"), "007f80ff");
      }
  }
};

TEST_P(KernelFileSystemExecution, OriginalDriverObservesDeclaredVolumes) {
  run(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
      "driver_filesystem_provider.sys");
}
TEST_P(KernelFileSystemExecution, GenuineWdkObservesDeclaredVolumes) {
#if !defined(NEVERD_WDM_FILESYSTEM_FIXTURE) ||                                 \
    !defined(NEVERD_WDM_FILESYSTEM_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK filesystem fixture was not supplied";
#else
  run(NEVERD_WDM_FILESYSTEM_FIXTURE);
  if (HasFatalFailure() || IsSkipped())
    return;
  run(NEVERD_WDM_FILESYSTEM_CFG_FIXTURE);
#endif
}
TEST_P(KernelFileSystemExecution, OriginalDriverOpensExistingFiles) {
  runOpens(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
           "driver_filesystem_provider.sys");
}
TEST_P(KernelFileSystemExecution, GenuineWdkOpensExistingFiles) {
#if !defined(NEVERD_WDM_FILESYSTEM_FIXTURE) ||                                 \
    !defined(NEVERD_WDM_FILESYSTEM_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK filesystem fixture was not supplied";
#else
  runOpens(NEVERD_WDM_FILESYSTEM_FIXTURE);
  if (HasFatalFailure() || IsSkipped())
    return;
  runOpens(NEVERD_WDM_FILESYSTEM_CFG_FIXTURE);
#endif
}
std::string fileSystemParameterName(
    const testing::TestParamInfo<KernelFileSystemExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}
INSTANTIATE_TEST_SUITE_P(
    Backends, KernelFileSystemExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    fileSystemParameterName);
} // namespace
} // namespace neverd::emulation
