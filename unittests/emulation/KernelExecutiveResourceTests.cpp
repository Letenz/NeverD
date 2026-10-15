//===- KernelExecutiveResourceTests.cpp - Resource ownership tests --------===//
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

namespace neverd::emulation {
namespace {
class KernelExecutiveResource : public testing::Test {
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
  std::vector<uint8_t> bytes(uint64_t Address, size_t Size = 104) {
    std::vector<uint8_t> Out(Size);
    success(Memory->read(Address, Out));
    return Out;
  }
  uint64_t pool(uint32_t Type = 0, uint64_t Size = 32) {
    return call("ExAllocatePoolWithTag", {Type, Size, Tag});
  }
  void init(uint64_t Address = Scratch) {
    call("ExInitializeResourceLite", {Address});
  }
  void erase(uint64_t Address = Scratch) {
    call("ExDeleteResourceLite", {Address});
  }
  void enter() { call("KeEnterCriticalRegion", {}); }
  void leave() { call("KeLeaveCriticalRegion", {}); }
  void thread(uint64_t I) {
    Model->enterExecution(Scratch + 0x8000 + I * 0x1000);
    Model->enterForeground();
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

TEST_F(KernelExecutiveResource,
       InitializationRegistrationAndDeletionAreAtomic) {
  for (uint64_t A : {Scratch, Scratch + 0x100, Scratch + 0x200}) {
    success(Memory->write(A, std::vector<uint8_t>(104, 0xa5)));
    init(A);
    EXPECT_EQ(bytes(A + 16, 88), std::vector<uint8_t>(88));
  }
  const uint64_t Head = read(Scratch + 8);
  EXPECT_EQ(read(Head), Scratch);
  EXPECT_EQ(read(Head + 8), Scratch + 0x200);
  EXPECT_EQ(read(Scratch), Scratch + 0x100);
  EXPECT_EQ(read(Scratch + 0x100 + 8), Scratch);
  const auto Before = bytes(Scratch, 0x300);
  EXPECT_NE(failure("ExInitializeResourceLite", {Scratch}).find("already"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch, 0x300), Before);
  // A successor on a separate page must be writable before any unlink store.
  init(Scratch + 0x1000);
  const auto Links = bytes(Scratch + 0x200);
  success(Memory->protect(Scratch + 0x1000, 4096, Read));
  EXPECT_NE(failure("ExDeleteResourceLite", {Scratch + 0x200}).find("writable"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch + 0x200), Links);
  success(Memory->protect(Scratch + 0x1000, 4096, Read | Write));
  erase(Scratch + 0x200);
  EXPECT_EQ(read(Scratch + 0x100), Scratch + 0x1000);
  EXPECT_EQ(read(Scratch + 0x1000 + 8), Scratch + 0x100);
  erase(Scratch);
  erase(Scratch + 0x100);
  erase(Scratch + 0x1000);
  EXPECT_EQ(read(Head), Head);
  EXPECT_EQ(read(Head + 8), Head);
}

TEST_F(KernelExecutiveResource,
       RecursiveAcquisitionAndConversionPreserveCounts) {
  init();
  EXPECT_NE(failure("ExAcquireResourceExclusiveLite", {Scratch, 1})
                .find("APCs disabled"),
            std::string::npos);
  enter();
  EXPECT_EQ(call("ExAcquireResourceExclusiveLite", {Scratch, 0x100000001}), 1u);
  EXPECT_EQ(call("ExTryToAcquireResourceExclusiveLite", {Scratch}), 1u);
  for (const char *Name :
       {"ExAcquireResourceSharedLite", "ExAcquireSharedStarveExclusive",
        "ExAcquireSharedWaitForExclusive"})
    EXPECT_EQ(call(Name, {Scratch, 0x100000000}), 1u);
  EXPECT_EQ(call("ExIsResourceAcquiredExclusiveLite", {Scratch}), 1u);
  EXPECT_EQ(call("ExIsResourceAcquiredSharedLite", {Scratch}), 5u);
  EXPECT_EQ(read(Scratch + 56, 4), 40u);
  call("ExConvertExclusiveToSharedLite", {Scratch});
  EXPECT_EQ(call("ExIsResourceAcquiredExclusiveLite", {Scratch}), 0u);
  EXPECT_EQ(call("ExIsResourceAcquiredSharedLite", {Scratch}), 5u);
  const auto Before = bytes(Scratch);
  EXPECT_EQ(call("ExTryToAcquireResourceExclusiveLite", {Scratch}), 0u);
  EXPECT_NE(
      failure("ExAcquireResourceExclusiveLite", {Scratch, 1}).find("waiting"),
      std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
  const uint64_t Thread = call("KeGetCurrentThread", {});
  for (uint64_t N = 5; N; --N) {
    EXPECT_EQ(call("ExIsResourceAcquiredSharedLite", {Scratch}), N);
    call("ExReleaseResourceForThreadLite", {Scratch, Thread});
  }
  EXPECT_EQ(read(Scratch + 24, 4), 0u);
  EXPECT_EQ(read(Scratch + 48), 0u);
  EXPECT_EQ(call("ExGetSharedWaiterCount", {Scratch}), 0u);
  EXPECT_EQ(call("ExGetExclusiveWaiterCount", {Scratch}), 0u);
  call("ExDisableResourceBoostLite", {Scratch});
  EXPECT_EQ(read(Scratch + 26, 2), 8u);
  call("ExReinitializeResourceLite", {Scratch});
  EXPECT_EQ(read(Scratch + 26, 2), 0u);
  leave();
  erase();
}

TEST_F(KernelExecutiveResource, SharedOwnersExpandAndRetainTheirOpaqueTable) {
  init();
  enter();
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  std::vector<KernelModel::ExecutionContext> Contexts{
      Model->captureExecutionContext()};
  uint64_t RetiredTable = 0;
  for (unsigned I = 1; I < 6; ++I) {
    thread(I);
    enter();
    EXPECT_EQ(call("ExAcquireResourceSharedLite", {Scratch, 1}), 1u);
    EXPECT_EQ(call("ExIsResourceAcquiredSharedLite", {Scratch}), 1u);
    EXPECT_EQ(read(Scratch + 64, 4), I + 1);
    if (I == 1)
      RetiredTable = read(Scratch + 16);
    Contexts.push_back(Model->captureExecutionContext());
  }
  const uint64_t Table = read(Scratch + 16);
  ASSERT_NE(Table, RetiredTable);
  EXPECT_NE(denied(RetiredTable, 1).find("freed"), std::string::npos);
  EXPECT_EQ(read(Table + 8, 4), 7u);
  EXPECT_NE(denied(Table, 16).find("opaque executive"), std::string::npos);
  for (unsigned I = 0; I < 6; ++I) {
    success(Model->restoreExecutionContext(Contexts[I]));
    EXPECT_EQ(call("ExIsResourceAcquiredSharedLite", {Scratch}), 1u);
    call("ExReleaseResourceLite", {Scratch});
    leave();
    EXPECT_EQ(read(Scratch + 64, 4), 5u - I);
  }
  call("ExReinitializeResourceLite", {Scratch});
  EXPECT_EQ(read(Scratch + 16), Table);
  erase();
  EXPECT_EQ(read(Scratch + 16), Table);
  EXPECT_NE(denied(Table, 1).find("freed"), std::string::npos);
}

TEST_F(KernelExecutiveResource,
       WaitForExclusiveKeepsTheInlineSlotReservedForActiveReaders) {
  init();
  enter();
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  std::vector<KernelModel::ExecutionContext> Contexts{
      Model->captureExecutionContext()};
  for (unsigned I = 1; I < 3; ++I) {
    thread(I);
    enter();
    call("ExAcquireSharedWaitForExclusive", {Scratch, 1});
    Contexts.push_back(Model->captureExecutionContext());
  }
  const uint64_t OldTable = read(Scratch + 16);
  success(Model->restoreExecutionContext(Contexts[0]));
  call("ExReleaseResourceLite", {Scratch});
  leave();
  thread(3);
  enter();
  call("ExAcquireSharedWaitForExclusive", {Scratch, 1});
  EXPECT_EQ(read(Scratch + 48), 0u);
  EXPECT_NE(read(Scratch + 16), OldTable);
  EXPECT_EQ(read(read(Scratch + 16) + 8, 4), 7u);
  call("ExReleaseResourceLite", {Scratch});
  leave();
  for (unsigned I = 1; I < 3; ++I) {
    success(Model->restoreExecutionContext(Contexts[I]));
    call("ExReleaseResourceLite", {Scratch});
    leave();
  }
  erase();
}

TEST_F(KernelExecutiveResource, FailedSharedTryRetainsTableButNoOwnership) {
  init();
  init(Scratch + 0x100);
  enter();
  call("ExAcquireResourceExclusiveLite", {Scratch, 1});
  call("ExAcquireResourceExclusiveLite", {Scratch + 0x100, 1});
  const auto Parent = Model->captureExecutionContext();
  const uint64_t Owner = call("KeGetCurrentThread", {});
  thread(1);
  enter();
  const auto Initial = bytes(Scratch);
  EXPECT_EQ(call("ExAcquireResourceExclusiveLite", {Scratch, 0}), 0u);
  EXPECT_EQ(bytes(Scratch), Initial);
  EXPECT_EQ(call("ExAcquireResourceSharedLite", {Scratch, 0}), 0u);
  const uint64_t Table = read(Scratch + 16);
  ASSERT_NE(Table, 0u);
  EXPECT_EQ(read(Table + 8, 4), 3u);
  EXPECT_EQ(read(Scratch + 48), Owner);
  EXPECT_EQ(call("ExIsResourceAcquiredSharedLite", {Scratch}), 0u);
  const auto WaitExclusiveBefore = bytes(Scratch + 0x100);
  EXPECT_EQ(call("ExAcquireSharedWaitForExclusive", {Scratch + 0x100, 0}), 0u);
  EXPECT_EQ(bytes(Scratch + 0x100), WaitExclusiveBefore);
  const auto Before = bytes(Scratch);
  EXPECT_NE(
      failure("ExAcquireResourceSharedLite", {Scratch, 1}).find("waiting"),
      std::string::npos);
  EXPECT_NE(failure("ExReleaseResourceForThreadLite", {Scratch, Owner})
                .find("foreign"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
  leave();
  success(Model->restoreExecutionContext(Parent));
  call("ExReleaseResourceLite", {Scratch});
  call("ExReleaseResourceLite", {Scratch + 0x100});
  leave();
  erase();
  erase(Scratch + 0x100);
}

TEST_F(KernelExecutiveResource, APCSuppressionCannotEndBeforeItsResources) {
  init();
  call("KeInitializeGuardedMutex", {Scratch + 0x1000});
  enter();
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  EXPECT_NE(failure("KeLeaveCriticalRegion", {}).find("executive resource"),
            std::string::npos);
  call("ExAcquireFastMutex", {Scratch + 0x1000});
  leave(); // APC_LEVEL now supplies suppression.
  const auto Before = bytes(Scratch + 0x1000, 56);
  EXPECT_NE(failure("ExReleaseFastMutex", {Scratch + 0x1000})
                .find("executive resource"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch + 0x1000, 56), Before);
  EXPECT_EQ(call("KeGetCurrentIrql", {}), 1u);
  call("ExReleaseResourceLite", {Scratch});
  call("ExReleaseFastMutex", {Scratch + 0x1000});
  call("KeInitializeMutex", {Scratch + 0x2000, 0});
  call("KeWaitForSingleObject", {Scratch + 0x2000, 0, 0, 0, 0});
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  EXPECT_NE(failure("KeReleaseMutex", {Scratch + 0x2000, 0})
                .find("executive resource"),
            std::string::npos);
  call("ExReleaseResourceLite", {Scratch});
  call("KeReleaseMutex", {Scratch + 0x2000, 0});
  call("KfRaiseIrql", {1});
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  EXPECT_NE(failure("KeLowerIrql", {0}).find("executive resource"),
            std::string::npos);
  call("ExReleaseResourceLite", {Scratch});
  call("KeLowerIrql", {0});
  erase();
}

TEST_F(KernelExecutiveResource,
       CombinedCriticalRegionCallsPreflightBothLifetimes) {
  init();
  init(Scratch + 0x100);
  for (const char *Name :
       {"ExEnterCriticalRegionAndAcquireResourceExclusive",
        "ExEnterCriticalRegionAndAcquireResourceShared",
        "ExEnterCriticalRegionAndAcquireSharedWaitForExclusive"}) {
    call(Name, {Scratch});
    EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
    call("ExAcquireResourceSharedLite", {Scratch + 0x100, 1});
    const auto Before = bytes(Scratch);
    EXPECT_NE(failure("ExReleaseResourceAndLeaveCriticalRegion", {Scratch})
                  .find("executive resource"),
              std::string::npos);
    EXPECT_EQ(bytes(Scratch), Before);
    call("ExReleaseResourceLite", {Scratch + 0x100});
    call("ExReleaseResourceAndLeaveCriticalRegion", {Scratch});
    EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  }
  erase();
  erase(Scratch + 0x100);
}

TEST_F(KernelExecutiveResource,
       OwnerPointerTransferPreservesCountAndStorageLifetime) {
  for (bool Shared : {false, true}) {
    init();
    const uint64_t Token = pool(0, 1);
    enter();
    const char *Acquire = Shared ? "ExAcquireResourceSharedLite"
                                 : "ExAcquireResourceExclusiveLite";
    call(Acquire, {Scratch, 1});
    call(Acquire, {Scratch, 1});
    EXPECT_NE(
        failure("ExSetResourceOwnerPointer", {Scratch, Token}).find("tagged"),
        std::string::npos);
    EXPECT_NE(failure("ExSetResourceOwnerPointerEx", {Scratch, Token | 3, 1})
                  .find("unsupported"),
              std::string::npos);
    if (Shared)
      call("ExSetResourceOwnerPointer", {Scratch, Token | 3});
    else
      call("ExSetResourceOwnerPointerEx", {Scratch, Token | 3, 0x100000000});
    leave();
    EXPECT_EQ(read(Scratch + 48), Token | 3);
    EXPECT_NE(failure("ExIsResourceAcquiredSharedLite", {Scratch})
                  .find("only release"),
              std::string::npos);
    EXPECT_NE(failure("ExFreePool", {Token}).find("owner pointer"),
              std::string::npos);
    success(Model->validateExecutionReturn(profile::StackBase, 0));
    thread(1);
    enter();
    EXPECT_NE(failure("ExReleaseResourceLite", {Scratch}).find("owner"),
              std::string::npos);
    call("ExReleaseResourceForThreadLite", {Scratch, Token | 3});
    EXPECT_NE(failure("ExFreePool", {Token}).find("owner pointer"),
              std::string::npos);
    call("ExReleaseResourceForThreadLite", {Scratch, Token | 3});
    leave();
    erase();
    call("ExFreePool", {Token});
    Model->enterExecution(profile::StackBase);
    Model->enterForeground();
  }
}

TEST_F(KernelExecutiveResource,
       TransferredSharedOwnerRestrictsAllOtherReleases) {
  init();
  const uint64_t Token = pool();
  enter();
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  const auto First = Model->captureExecutionContext();
  thread(1);
  enter();
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  const uint64_t SecondOwner = call("KeGetCurrentThread", {});
  const auto Second = Model->captureExecutionContext();
  success(Model->restoreExecutionContext(First));
  call("ExSetResourceOwnerPointerEx", {Scratch, Token | 3, 0});
  leave();
  success(Model->restoreExecutionContext(Second));
  const auto Before = bytes(Scratch);
  EXPECT_NE(failure("ExReleaseResourceLite", {Scratch}).find("only release"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
  call("ExReleaseResourceForThreadLite", {Scratch, SecondOwner});
  call("ExReleaseResourceForThreadLite", {Scratch, Token | 3});
  leave();
  erase();
  call("ExFreePool", {Token});
}

TEST_F(KernelExecutiveResource, OpaqueStorageAndMdlBorrowingCannotOverlap) {
  const uint64_t R = pool(0, 104);
  const uint64_t MDL = call("IoAllocateMdl", {R, 104, 0, 0, 0});
  call("MmBuildMdlForNonPagedPool", {MDL});
  const auto Before = bytes(R);
  EXPECT_NE(failure("ExInitializeResourceLite", {R}).find("borrow"),
            std::string::npos);
  EXPECT_EQ(bytes(R), Before);
  call("IoFreeMdl", {MDL});
  init(R);
  EXPECT_NE(denied(R, 104).find("opaque executive"), std::string::npos);
  EXPECT_NE(denied(R, 104, true).find("opaque executive"), std::string::npos);
  EXPECT_NE(failure("ExFreePool", {R}).find("opaque executive"),
            std::string::npos);
  const uint64_t MDL2 = call("IoAllocateMdl", {R, 104, 0, 0, 0});
  EXPECT_NE(
      failure("MmBuildMdlForNonPagedPool", {MDL2}).find("opaque executive"),
      std::string::npos);
  EXPECT_NE(
      failure("MmProbeAndLockPages", {MDL2, 0, 1}).find("opaque executive"),
      std::string::npos);
  call("IoFreeMdl", {MDL2});
  erase(R);
  call("ExFreePool", {R});
}

TEST_F(KernelExecutiveResource,
       MalformedPagedReadonlyAndCorruptedStorageFailClearly) {
  const uint64_t Paged = pool(1, 104);
  EXPECT_NE(failure("ExInitializeResourceLite", {Paged}).find("nonpaged"),
            std::string::npos);
  call("ExFreePool", {Paged});
  EXPECT_NE(failure("ExInitializeResourceLite", {Scratch + 1}).find("aligned"),
            std::string::npos);
  const auto DriverBefore = bytes(Model->driverObject() + 0x50);
  EXPECT_FALSE(
      failure("ExInitializeResourceLite", {Model->driverObject() + 0x50})
          .empty());
  EXPECT_EQ(bytes(Model->driverObject() + 0x50), DriverBefore);
  init();
  const auto Before = bytes(Scratch);
  for (uint64_t Offset : {0, 16, 24, 32, 48, 56, 64, 80, 96}) {
    write(Scratch + Offset, read(Scratch + Offset) ^ 1);
    const auto Bad = bytes(Scratch);
    EXPECT_NE(failure("ExGetSharedWaiterCount", {Scratch}).find("modified"),
              std::string::npos);
    EXPECT_EQ(bytes(Scratch), Bad);
    success(Memory->write(Scratch, Before));
  }
  success(Memory->protect(Scratch, 4096, Read));
  EXPECT_EQ(call("ExGetSharedWaiterCount", {Scratch}), 0u);
  enter();
  EXPECT_NE(
      failure("ExAcquireResourceSharedLite", {Scratch, 1}).find("writable"),
      std::string::npos);
  leave();
  success(Memory->protect(Scratch, 4096, Read | Write));
  call("KfRaiseIrql", {2});
  call("ExReinitializeResourceLite", {Scratch});
  EXPECT_FALSE(failure("ExDeleteResourceLite", {Scratch}).empty());
  EXPECT_FALSE(failure("ExAcquireResourceSharedLite", {Scratch, 0}).empty());
  call("KeLowerIrql", {0});
  erase();
}

TEST_F(KernelExecutiveResource,
       LiveResourcesBlockUnloadAndOwnedThreadTermination) {
  success(Model->captureUnpackBaseline());
  init();
  auto Reasons = Model->unpackDependencies();
  ASSERT_TRUE(bool(Reasons)) << llvm::toString(Reasons.takeError());
  EXPECT_NE(std::find(Reasons->begin(), Reasons->end(),
                      "live kernel executive resources"),
            Reasons->end());
  call("PsCreateSystemThread",
       {Scratch + 0x1000, 0x1fffff, 0, 0, 0, 0x180001000, 0});
  const uint64_t Handle = read(Scratch + 0x1000);
  auto Invocation = Model->nextScheduled(false);
  ASSERT_TRUE(bool(Invocation)) << llvm::toString(Invocation.takeError());
  ASSERT_TRUE(*Invocation);
  Model->enterExecution(Scratch + 0x8000, (**Invocation).ID);
  call("ExAcquireResourceSharedLite", {Scratch, 1});
  EXPECT_NE(failure("PsTerminateSystemThread", {0}).find("executive resource"),
            std::string::npos);
  EXPECT_FALSE(Model->takeThreadTermination());
  EXPECT_NE(failure("ExDeleteResourceLite", {Scratch}).find("owned"),
            std::string::npos);
  EXPECT_NE(failure("ExReinitializeResourceLite", {Scratch}).find("owned"),
            std::string::npos);
  call("ExReleaseResourceLite", {Scratch});
  call("PsTerminateSystemThread", {0});
  EXPECT_EQ(Model->takeThreadTermination(), std::optional<uint32_t>(0));
  success(Model->finishScheduled((**Invocation).ID));
  call("ZwClose", {Handle});
  write(Model->driverObject() + 0x68, 0x180001000);
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("live executive resources"),
            std::string::npos);
  erase();
  success(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

// END LOCAL RESOURCE ORACLE TEST
} // namespace
} // namespace neverd::emulation
