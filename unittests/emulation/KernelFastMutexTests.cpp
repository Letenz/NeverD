//===- KernelFastMutexTests.cpp - Fast mutex ownership boundaries
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelFastMutex : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Tag = 0x74734c4e;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelExportRegistry> Exports;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  void initialize(const DriverOptions &Options = {}) {
    Model.reset();
    auto Backend = UnicornBackend::create(4 * 1024 * 1024);
    ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
    Memory = std::move(*Backend);
    success(Memory->map(Scratch, 0x10000, Read | Write));
    Exports = std::make_unique<KernelExportRegistry>();
    success(Exports->initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, Exports.get());
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    success(Model->initialize(Image, Options));
  }
  void SetUp() override {
    initialize();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (!V) {
      ADD_FAILURE() << Name.str() << ": " << llvm::toString(V.takeError());
      return 0;
    }
    return *V;
  }
  std::string failure(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (V) {
      ADD_FAILURE() << Name.str() << " unexpectedly succeeded";
      return {};
    }
    return llvm::toString(V.takeError());
  }
  uint64_t read(uint64_t Address, unsigned Size = 8) {
    auto V = Memory->readInteger(Address, Size);
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return 0;
    }
    return *V;
  }
  void write(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    success(Memory->writeInteger(Address, Value, Size));
  }
  std::vector<uint8_t> bytes(uint64_t Address, size_t Size = 56) {
    std::vector<uint8_t> Out(Size);
    success(Memory->read(Address, Out));
    return Out;
  }
  uint64_t pool(uint32_t Type = 0, uint64_t Size = 32) {
    return call("ExAllocatePoolWithTag", {Type, Size, Tag});
  }
  void init(uint64_t Address = Scratch, bool Guarded = true) {
    if (Guarded) {
      call("KeInitializeGuardedMutex", {Address});
      return;
    }
    // ExInitializeFastMutex is the WDK macro, including its event API call.
    write(Address, 1, 4);
    write(Address + 8, 0);
    write(Address + 16, 0, 4);
    call("KeInitializeEvent", {Address + 24, 1, 0});
  }
  std::string denied(uint64_t Address, uint32_t Size, bool Write = false) {
    auto E = Model->validateGuestAccess(Address, Size, Write);
    if (!E) {
      ADD_FAILURE() << "guest access unexpectedly allowed";
      return {};
    }
    return llvm::toString(std::move(E));
  }
};

TEST_F(KernelFastMutex, GuardedInitializerPreservesOldIrqlAndPadding) {
  for (uint8_t Pattern : {0, 0x55, 0xff}) {
    success(Memory->write(Scratch, std::vector<uint8_t>(56, Pattern)));
    init();
    EXPECT_EQ(read(Scratch, 4), 1u);
    EXPECT_EQ(read(Scratch + 4, 4), uint32_t(Pattern) * 0x01010101u);
    EXPECT_EQ(read(Scratch + 8), 0u);
    EXPECT_EQ(read(Scratch + 16, 4), 0u);
    EXPECT_EQ(read(Scratch + 20, 4), uint32_t(Pattern) * 0x01010101u);
    EXPECT_EQ(read(Scratch + 24, 2), 1u);
    EXPECT_EQ(read(Scratch + 26, 1), 6u);
    EXPECT_EQ(read(Scratch + 27, 1), Pattern);
    EXPECT_EQ(read(Scratch + 28, 4), 0u);
    EXPECT_EQ(read(Scratch + 32), Scratch + 32);
    EXPECT_EQ(read(Scratch + 40), Scratch + 32);
    EXPECT_EQ(read(Scratch + 48), uint64_t(Pattern) * 0x0101010101010101u);
    EXPECT_EQ(call("KeReadStateEvent", {Scratch + 24}), 0u);
    EXPECT_NE(denied(Scratch + 24, 24).find("opaque dispatcher"),
              std::string::npos);
  }
}

TEST_F(KernelFastMutex, NormalAndTryAcquisitionPreserveSurroundingBytes) {
  for (bool GuardedInit : {false, true})
    for (bool GuardedAPI : {false, true})
      for (bool Try : {false, true})
        for (uint64_t IRQL : {0, 1}) {
          success(Memory->write(Scratch, std::vector<uint8_t>(56, 0xa5)));
          init(Scratch, GuardedInit);
          const auto Before = bytes(Scratch);
          const uint64_t Thread = call("KeGetCurrentThread", {});
          call("KfRaiseIrql", {IRQL});
          const char *Acquire = GuardedAPI ? (Try ? "KeTryToAcquireGuardedMutex"
                                                  : "KeAcquireGuardedMutex")
                                           : (Try ? "ExTryToAcquireFastMutex"
                                                  : "ExAcquireFastMutex");
          EXPECT_EQ(call(Acquire, {Scratch}), Try ? 1u : 0u);
          EXPECT_EQ(call("KeGetCurrentIrql", {}), 1u);
          EXPECT_EQ(call("KeAreAllApcsDisabled", {}), 1u);
          EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
          EXPECT_EQ(read(Scratch, 4), 0u);
          EXPECT_EQ(read(Scratch + 8), Thread);
          EXPECT_EQ(read(Scratch + 48, 4), IRQL);
          EXPECT_EQ(
              bytes(Scratch + 16, 32),
              std::vector<uint8_t>(Before.begin() + 16, Before.begin() + 48));
          EXPECT_EQ(bytes(Scratch + 52, 4), std::vector<uint8_t>(4, 0xa5));
          call(GuardedAPI ? "KeReleaseGuardedMutex" : "ExReleaseFastMutex",
               {Scratch});
          EXPECT_EQ(read(Scratch, 4), 1u);
          EXPECT_EQ(read(Scratch + 8), 0u);
          EXPECT_EQ(call("KeGetCurrentIrql", {}), IRQL);
          call("KeLowerIrql", {0});
        }
}

TEST_F(KernelFastMutex, ContentionTryAndForeignReleaseDoNotMutateLock) {
  init();
  call("ExAcquireFastMutex", {Scratch});
  const auto Before = bytes(Scratch);
  EXPECT_EQ(call("KeTryToAcquireGuardedMutex", {Scratch}), 0u);
  EXPECT_NE(failure("ExAcquireFastMutex", {Scratch}).find("AutoBoost"),
            std::string::npos);
  EXPECT_NE(failure("ExReleaseFastMutexUnsafe", {Scratch}).find("variant"),
            std::string::npos);
  const auto Parent = Model->captureExecutionContext();
  Model->enterExecution(Scratch + 0x8000);
  Model->enterForeground();
  EXPECT_EQ(call("ExTryToAcquireFastMutex", {Scratch}), 0u);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), 0u);
  EXPECT_NE(failure("KeAcquireGuardedMutex", {Scratch}).find("AutoBoost"),
            std::string::npos);
  EXPECT_NE(failure("KeReleaseGuardedMutex", {Scratch}).find("owning thread"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
  success(Model->restoreExecutionContext(Parent));
  call("ExReleaseFastMutex", {Scratch});
}

TEST_F(KernelFastMutex, UnsafeFormsKeepIrqlAndRequireTheirDisableContext) {
  init();
  write(Scratch + 48, 0x12345678, 4);
  EXPECT_NE(failure("ExAcquireFastMutexUnsafe", {Scratch}).find("APC-disable"),
            std::string::npos);
  call("KeEnterCriticalRegion", {});
  EXPECT_NE(
      failure("KeAcquireGuardedMutexUnsafe", {Scratch}).find("APC-disable"),
      std::string::npos);
  call("ExAcquireFastMutexUnsafe", {Scratch});
  EXPECT_EQ(call("KeGetCurrentIrql", {}), 0u);
  EXPECT_EQ(read(Scratch + 48, 4), 0x12345678u);
  EXPECT_NE(failure("KeLeaveCriticalRegion", {}).find("unsafe fast mutex"),
            std::string::npos);
  EXPECT_NE(failure("ExReleaseFastMutex", {Scratch}).find("variant"),
            std::string::npos);
  const auto Parent = Model->captureExecutionContext();
  Model->enterExecution(Scratch + 0x8000, profile::StackBase);
  success(Model->validateExecutionReturn(Scratch + 0x8000, 0, true));
  call("ExReleaseFastMutexUnsafe", {Scratch});
  success(Model->restoreExecutionContext(Parent));
  call("KeLeaveCriticalRegion", {});
  call("KeEnterGuardedRegion", {});
  call("KeAcquireGuardedMutexUnsafe", {Scratch});
  EXPECT_NE(failure("KeLeaveGuardedRegion", {}).find("unsafe fast mutex"),
            std::string::npos);
  call("KeReleaseGuardedMutexUnsafe", {Scratch});
  call("KeLeaveGuardedRegion", {});
  call("KfRaiseIrql", {1});
  call("KeAcquireGuardedMutexUnsafe", {Scratch});
  EXPECT_NE(failure("KeLowerIrql", {0}).find("unsafe fast mutex"),
            std::string::npos);
  call("KeReleaseGuardedMutexUnsafe", {Scratch});
  call("KeLowerIrql", {0});
  EXPECT_EQ(read(Scratch + 48, 4), 0x12345678u);
}

TEST_F(KernelFastMutex, NestedLocksCannotRestoreBelowAnotherHoldersIrql) {
  init();
  init(Scratch + 0x100);
  call("ExAcquireFastMutex", {Scratch});
  call("KeAcquireGuardedMutex", {Scratch + 0x100});
  const auto Before = bytes(Scratch);
  EXPECT_NE(failure("ExReleaseFastMutex", {Scratch}).find("cannot lower"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
  call("KeReleaseGuardedMutex", {Scratch + 0x100});
  call("ExReleaseFastMutex", {Scratch});
  call("KfRaiseIrql", {1});
  call("ExAcquireFastMutex", {Scratch});
  EXPECT_NE(failure("KeLowerIrql", {0}).find("cannot lower"),
            std::string::npos);
  call("ExReleaseFastMutex", {Scratch});
  call("KeLowerIrql", {0});
  EXPECT_EQ(call("KeGetCurrentIrql", {}), 0u);
}

TEST_F(KernelFastMutex, HeldObjectsRejectMutationBorrowingAndRetirement) {
  const uint64_t M = pool(0, 56);
  init(M);
  call("KeEnterCriticalRegion", {});
  call("ExAcquireFastMutexUnsafe", {M});
  const auto Before = bytes(M);
  EXPECT_NE(failure("ExFreePool", {M}).find("held fast mutex"),
            std::string::npos);
  EXPECT_NE(failure("KeInitializeGuardedMutex", {M}).find("held fast mutex"),
            std::string::npos);
  EXPECT_NE(failure("KeSetEvent", {M + 24, 0, 0}).find("held fast mutex"),
            std::string::npos);
  EXPECT_NE(failure("memset", {M, 0, 56}).find("held fast mutex"),
            std::string::npos);
  auto E = Model->validateExecutionReturn(profile::StackBase, 0);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("owned fast mutex"),
            std::string::npos);
  const uint64_t MDL = call("IoAllocateMdl", {M, 56, 0, 0, 0});
  EXPECT_NE(failure("MmBuildMdlForNonPagedPool", {MDL}).find("held fast mutex"),
            std::string::npos);
  EXPECT_NE(failure("MmProbeAndLockPages", {MDL, 0, 1}).find("held fast mutex"),
            std::string::npos);
  EXPECT_EQ(bytes(M), Before);
  call("IoFreeMdl", {MDL});
  call("ExReleaseFastMutexUnsafe", {M});
  call("KeLeaveCriticalRegion", {});
  call("ExFreePool", {M});
}

TEST_F(KernelFastMutex, ExistingMdlBorrowPreventsInitializationAndAcquire) {
  const uint64_t M = pool(0, 56);
  init(M);
  const uint64_t MDL = call("IoAllocateMdl", {M, 56, 0, 0, 0});
  call("MmBuildMdlForNonPagedPool", {MDL});
  const auto Before = bytes(M);
  EXPECT_NE(failure("ExAcquireFastMutex", {M}).find("borrow"),
            std::string::npos);
  EXPECT_NE(failure("KeInitializeGuardedMutex", {M}).find("borrow"),
            std::string::npos);
  EXPECT_EQ(bytes(M), Before);
  call("IoFreeMdl", {MDL});
  call("ExAcquireFastMutex", {M});
  call("ExReleaseFastMutex", {M});
  call("ExFreePool", {M});
}

TEST_F(KernelFastMutex, InvalidStorageAndCorruptionFailBeforeStateChanges) {
  const uint64_t Opaque = Model->driverObject() + 0x50;
  const auto ObjectBefore = bytes(Opaque);
  EXPECT_FALSE(failure("KeInitializeGuardedMutex", {Opaque}).empty());
  EXPECT_EQ(bytes(Opaque), ObjectBefore);
  const uint64_t Paged = pool(1, 56);
  EXPECT_NE(failure("KeInitializeGuardedMutex", {Paged}).find("nonpaged"),
            std::string::npos);
  call("ExFreePool", {Paged});
  EXPECT_NE(failure("KeInitializeGuardedMutex", {Scratch + 1}).find("aligned"),
            std::string::npos);
  init();
  success(Memory->protect(Scratch, 4096, Read));
  EXPECT_NE(failure("ExAcquireFastMutex", {Scratch}).find("writable"),
            std::string::npos);
  success(Memory->protect(Scratch, 4096, Read | Write));
  write(Scratch, 0xffffffff, 4);
  EXPECT_NE(failure("ExTryToAcquireFastMutex", {Scratch}).find("waiter count"),
            std::string::npos);
  write(Scratch, 1, 4);
  call("ExAcquireFastMutex", {Scratch});
  for (auto [Offset, Width] : {std::pair{0u, 4u}, {8u, 8u}, {48u, 4u}}) {
    const uint64_t Value = read(Scratch + Offset, Width);
    write(Scratch + Offset, Value + 1, Width);
    const auto Before = bytes(Scratch);
    EXPECT_NE(failure("ExReleaseFastMutex", {Scratch}).find("modified"),
              std::string::npos);
    EXPECT_EQ(bytes(Scratch), Before);
    write(Scratch + Offset, Value, Width);
  }
  call("ExReleaseFastMutex", {Scratch});
  call("KfRaiseIrql", {2});
  EXPECT_FALSE(failure("ExAcquireFastMutex", {Scratch}).empty());
  call("KeLowerIrql", {0});
}

TEST_F(KernelFastMutex, RequiresTheDispatchersSynchronizationEvent) {
  write(Scratch, 1, 4);
  EXPECT_NE(
      failure("ExAcquireFastMutex", {Scratch}).find("synchronization event"),
      std::string::npos);
  call("KeInitializeEvent", {Scratch + 24, 0, 0});
  EXPECT_NE(
      failure("ExAcquireFastMutex", {Scratch}).find("synchronization event"),
      std::string::npos);
  call("KeInitializeEvent", {Scratch + 24, 1, 1});
  EXPECT_NE(failure("ExAcquireFastMutex", {Scratch}).find("nonsignaled"),
            std::string::npos);
  init(Scratch, false);
  call("ExAcquireFastMutex", {Scratch});
  call("ExReleaseFastMutex", {Scratch});
}

TEST_F(KernelFastMutex, HeldLocksBlockUnloadAndRetainRecoveryDependency) {
  success(Model->captureUnpackBaseline());
  init();
  call("KeEnterGuardedRegion", {});
  call("KeAcquireGuardedMutexUnsafe", {Scratch});
  auto Reasons = Model->unpackDependencies();
  ASSERT_TRUE(bool(Reasons)) << llvm::toString(Reasons.takeError());
  EXPECT_NE(std::find(Reasons->begin(), Reasons->end(),
                      "unreconstructed kernel or environment effects"),
            Reasons->end());
  write(Model->driverObject() + 0x68, 0x180001000);
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("held fast mutex"),
            std::string::npos);
  call("KeReleaseGuardedMutexUnsafe", {Scratch});
  call("KeLeaveGuardedRegion", {});
  success(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(KernelFastMutex, OwningSystemThreadMustReleaseBeforeTermination) {
  init();
  call("PsCreateSystemThread",
       {Scratch + 0x1000, 0x1fffff, 0, 0, 0, 0x180001000, 0});
  const uint64_t Handle = read(Scratch + 0x1000);
  auto Invocation = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  ASSERT_TRUE(*Invocation);
  Model->enterExecution(Scratch + 0x8000, (**Invocation).ID);
  // System threads begin in a critical region; Unsafe preserves PASSIVE.
  call("ExAcquireFastMutexUnsafe", {Scratch});
  const auto Before = bytes(Scratch);
  EXPECT_NE(
      failure("PsTerminateSystemThread", {0}).find("retains a fast mutex"),
      std::string::npos);
  EXPECT_FALSE(Model->takeThreadTermination());
  EXPECT_EQ(bytes(Scratch), Before);
  call("ExReleaseFastMutexUnsafe", {Scratch});
  call("PsTerminateSystemThread", {0});
  EXPECT_EQ(Model->takeThreadTermination(), std::optional<uint32_t>(0));
  success(Model->finishScheduled((**Invocation).ID));
  call("ZwClose", {Handle});
}

} // namespace
} // namespace neverd::emulation
