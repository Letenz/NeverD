//===- KernelSnapshotTests.cpp - Terminal driver object observations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelAPINames.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/CPU.h"

#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint64_t Code = 0xfffff80140001000;
constexpr uint64_t Scratch = 0x60000000;
constexpr uint64_t Missing = 0x12345000;
constexpr uint64_t PageSize = 4096;
constexpr uint64_t MemoryLimit = 16 * 1024 * 1024;

class KernelSnapshot : public testing::TestWithParam<ExecutionBackendKind> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t First = 0, Second = 0;
  unsigned Reads = 0, Writes = 0;

  void ok(llvm::Error Error) {
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name.str(), Arguments));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(CPU->writeInteger(Address, Value, Width));
  }
  void SetUp() override {
    auto Backend = createExecutionBackend(
        GetParam(), ExecutionContract::CheckedX64, MemoryLimit);
    if (!Backend) {
      auto E = Backend.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(GetParam(), GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(Backend->CPU);
    ok(CPU->map(Code, PageSize, Read | Write | Execute));
    ok(CPU->map(Scratch, PageSize, Read | Write));
    Model = std::make_unique<KernelModel>(*CPU, Result);
    DriverImage Image;
    Image.Base = Code - PageSize;
    Image.Entry = Code;
    Image.Size = 3 * PageSize;
    ok(Model->initialize(Image, DriverOptions{}));
    for (auto *Device : {&First, &Second}) {
      ASSERT_EQ(call(kernel_api::IoCreateDevice,
                     {Model->driverObject(), 32, 0, 0x22, 0, 0, Scratch}),
                0u);
      *Device = take(CPU->readInteger(Scratch, 8));
      ASSERT_NE(*Device, 0u);
    }
    EXPECT_EQ(call(kernel_api::IoAttachDeviceToDeviceStack, {Second, First}),
              First);
    put(Model->driverObject() + DriverUnloadOffset, Code + 0x100);
    const auto Extension = take(
        CPU->readInteger(Model->driverObject() + DriverExtensionOffset, 8));
    put(Extension + DriverAddDeviceOffset, Code + 0x200);
    const auto Dispatch = Model->driverObject() + DriverDispatchOffset;
    ok(Model->validateGuestAccess(Dispatch, 8, true));
    put(Dispatch, Code + 0x300);
    ok(Model->snapshot());
  }
  void stop(bool Unsupported = false) {
    const uint8_t Load[] = {0x48, 0x8b, 0x01}; // mov rax, [rcx]
    const uint8_t UD2[] = {0x0f, 0x0b};
    ok(CPU->write(Code,
                  Unsupported ? llvm::ArrayRef(UD2) : llvm::ArrayRef(Load)));
    ok(CPU->setReg(X64Register::CX, Missing));
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t, uint32_t) { ++Reads; };
    Hooks.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
    ok(CPU->installHooks(std::move(Hooks)));
    const auto Exit = take(CPU->runUntilExit(Code, 1000000));
    EXPECT_EQ(Exit.Kind, Unsupported ? ExecutionExitKind::UnsupportedOperation
                                     : ExecutionExitKind::GuestFault)
        << Exit.Diagnostic;
    ASSERT_TRUE(CPU->fault());
    EXPECT_EQ(CPU->fault()->Kind, Unsupported
                                      ? BackendFaultKind::InvalidInstruction
                                      : BackendFaultKind::UnmappedMemory);
  }
  void sameFault(const BackendFault &Before) {
    const auto After = CPU->fault();
    ASSERT_TRUE(After);
    auto Fields = [](const BackendFault &F) {
      return std::tie(F.Kind, F.PC, F.Address, F.Size, F.Access, F.Interrupt,
                      F.ErrorCode, F.Cause);
    };
    EXPECT_EQ(Fields(*After), Fields(Before));
  }
  void snapshotAfterFault(bool Unsupported) {
    stop(Unsupported);
    ASSERT_TRUE(CPU->fault());
    const auto Fault = *CPU->fault();
    const auto Accesses = std::pair(Reads, Writes);
    Result.Devices.clear();
    Result.DriverUnload = 0;
    Result.AddDevice = 0;
    Result.MajorFunctions.fill(0);
    for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
      ok(Model->snapshot());
      ASSERT_EQ(Result.Devices.size(), 2u);
      EXPECT_EQ(Result.Devices[0].Address, Second);
      EXPECT_EQ(Result.Devices[1].Address, First);
      EXPECT_EQ(Result.Devices[0].Type, 0x22u);
      EXPECT_EQ(Result.DriverUnload, Code + 0x100);
      EXPECT_EQ(Result.AddDevice, Code + 0x200);
      EXPECT_EQ(Result.MajorFunctions[0], Code + 0x300);
      EXPECT_EQ(Result.MajorFunctions[1], 0u);
    }
    EXPECT_EQ(std::pair(Reads, Writes), Accesses);
    auto Read = CPU->readInteger(Model->driverObject(), 8);
    ASSERT_FALSE(bool(Read));
    llvm::consumeError(Read.takeError());
    auto Write = CPU->writeInteger(Model->driverObject(), 0, 8);
    ASSERT_TRUE(bool(Write));
    llvm::consumeError(std::move(Write));
    sameFault(Fault);
  }
  void rejectsCorruption(llvm::StringRef Expected) {
    auto Before = Model->snapshot();
    ASSERT_TRUE(bool(Before));
    const auto Diagnostic = llvm::toString(std::move(Before));
    EXPECT_NE(Diagnostic.find(Expected.str()), std::string::npos);
    stop();
    ASSERT_TRUE(CPU->fault());
    const auto Fault = *CPU->fault();
    auto After = Model->snapshot();
    ASSERT_TRUE(bool(After));
    EXPECT_EQ(llvm::toString(std::move(After)), Diagnostic);
    sameFault(Fault);
  }
};

TEST_P(KernelSnapshot, MemoryFaultRetainsCommittedObjectsAndFirstFault) {
  snapshotAfterFault(false);
}
TEST_P(KernelSnapshot, UnsupportedInstructionRetainsCommittedObjects) {
  snapshotAfterFault(true);
}
TEST_P(KernelSnapshot, CorruptedDeviceIdentityStillFailsAfterFault) {
  put(First + DeviceDriverOffset, 0);
  rejectsCorruption("DEVICE_OBJECT identity");
}
TEST_P(KernelSnapshot, CyclicDeviceListStillFailsAfterFault) {
  put(First + DeviceNext, Second);
  rejectsCorruption("cycle in DRIVER_OBJECT");
}
TEST_P(KernelSnapshot, CorruptedDriverExtensionStillFailsAfterFault) {
  put(Model->driverObject() + DriverExtensionOffset, Scratch);
  rejectsCorruption("DriverExtension was corrupted");
}
TEST_P(KernelSnapshot, PartialDispatchPointerStillFailsAfterFault) {
  const auto Dispatch = Model->driverObject() + DriverDispatchOffset + 8;
  ok(Model->validateGuestAccess(Dispatch, 4, true));
  put(Dispatch, 0x1234, 4);
  rejectsCorruption("partially initialized dispatch pointer");
}
INSTANTIATE_TEST_SUITE_P(
    Native, KernelSnapshot,
    testing::Values(ExecutionBackendKind::Unicorn, ExecutionBackendKind::KVM,
                    ExecutionBackendKind::WHP, ExecutionBackendKind::HVF),
    [](const testing::TestParamInfo<ExecutionBackendKind> &P) {
      return std::string(executionBackendName(P.param)) + "_checked";
    });
} // namespace
} // namespace neverd::emulation
