//===- KernelLookasideTests.cpp - Lookaside ownership boundaries ----------===//
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
class KernelLookaside : public testing::Test {
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
  void SetUp() override { initialize(); }
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
  std::vector<uint8_t> bytes(uint64_t Address, size_t Size = 96) {
    std::vector<uint8_t> Out(Size);
    success(Memory->read(Address, Out));
    return Out;
  }
  uint64_t pool(uint32_t Type = 0, uint64_t Size = 32) {
    return call("ExAllocatePoolWithTag", {Type, Size, Tag});
  }
  void init(uint64_t Address, bool Paged = false, uint64_t Size = 32,
            uint32_t Flags = 0) {
    call(Paged ? "ExInitializePagedLookasideList"
               : "ExInitializeNPagedLookasideList",
         {Address, 0, 0, Flags, Size, Tag, 0});
  }
  void erase(uint64_t Address, bool Paged = false) {
    call(Paged ? "ExDeletePagedLookasideList" : "ExDeleteNPagedLookasideList",
         {Address});
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

TEST_F(KernelLookaside, LayoutRegistrationAndRetirementPreserveReservedBytes) {
  for (uint64_t L : {Scratch, Scratch + 0x100, Scratch + 0x200})
    success(Memory->write(L, std::vector<uint8_t>(96, 0xa5)));
  init(Scratch, false, 0x100000020, 512);
  EXPECT_EQ(read(Scratch), 0u);
  EXPECT_EQ(read(Scratch + 8), 0u);
  EXPECT_EQ(read(Scratch + 16, 2), 4u);
  EXPECT_EQ(read(Scratch + 18, 2), 256u);
  EXPECT_EQ(read(Scratch + 20), 0u);
  EXPECT_EQ(read(Scratch + 28), 0u);
  EXPECT_EQ(read(Scratch + 36, 4), 512u);
  EXPECT_EQ(read(Scratch + 40, 4), Tag);
  EXPECT_EQ(read(Scratch + 44, 4), 32u);
  ASSERT_NE(Exports->lookup(read(Scratch + 48)), nullptr);
  EXPECT_EQ(Exports->lookup(read(Scratch + 48))->Name, "ExAllocatePoolWithTag");
  ASSERT_NE(Exports->lookup(read(Scratch + 56)), nullptr);
  EXPECT_EQ(Exports->lookup(read(Scratch + 56))->Name, "ExFreePool");
  EXPECT_EQ(read(Scratch + 80), 0u);
  EXPECT_EQ(read(Scratch + 88), 0xa5a5a5a5a5a5a5a5u);
  const uint64_t Head = read(Scratch + 64);
  EXPECT_EQ(read(Scratch + 72), Head);
  EXPECT_EQ(read(Head), Scratch + 64);
  EXPECT_NE(denied(Head, 8).find("private"), std::string::npos);
  init(Scratch + 0x100, true);
  EXPECT_NE(read(Scratch + 0x140), Head);
  init(Scratch + 0x200);
  EXPECT_EQ(read(Scratch + 64), Scratch + 0x240);
  EXPECT_EQ(read(Scratch + 0x248), Scratch + 64);
  EXPECT_EQ(read(Head + 8), Scratch + 0x240);
  erase(Scratch);
  EXPECT_EQ(read(Scratch + 16, 2), 0u);
  EXPECT_EQ(read(Scratch + 64), UINT64_MAX);
  EXPECT_EQ(read(Scratch + 72), Head);
  EXPECT_EQ(read(Scratch + 0x248), Head);
  EXPECT_EQ(read(Head), Scratch + 0x240);
  erase(Scratch + 0x200);
  erase(Scratch + 0x100, true);
  EXPECT_EQ(read(Head), Head);
  EXPECT_EQ(read(Head + 8), Head);
  init(Scratch);
  erase(Scratch);
}

TEST_F(KernelLookaside, CachedEntriesUseExistingPoolAndSListLifetimes) {
  for (bool Paged : {false, true})
    for (uint64_t Size : {8, 16, 32, 256}) {
      init(Scratch, Paged, Size);
      const uint64_t A = pool(Paged, Size), B = pool(Paged, Size);
      call("ExpInterlockedPushEntrySList", {Scratch, A});
      call("ExpInterlockedPushEntrySList", {Scratch, B});
      EXPECT_EQ(call("ExQueryDepthSList", {Scratch}), 2u);
      EXPECT_NE(failure("ExFreePool", {A}).find("cached"), std::string::npos);
      EXPECT_EQ(call("ExpInterlockedPopEntrySList", {Scratch}), B);
      call("ExFreePool", {B});
      const uint64_t Sequence = read(Scratch) & ~uint64_t(0xffff);
      erase(Scratch, Paged);
      EXPECT_EQ(read(Scratch), Sequence);
      EXPECT_EQ(read(Scratch + 8), 0u);
      EXPECT_NE(denied(A, 1).find("freed"), std::string::npos);
      EXPECT_NE(failure("ExFreePool", {A}).find("already freed"),
                std::string::npos);
    }
}

TEST_F(KernelLookaside, RefusedInitializationPreservesStorage) {
  success(Memory->write(Scratch, std::vector<uint8_t>(96, 0xa5)));
  const auto Before = bytes(Scratch);
  for (auto A : {std::array<uint64_t, 7>{Scratch, 1, 0, 0, 32, Tag, 0},
                 {Scratch, 0, 1, 0, 32, Tag, 0},
                 {Scratch, 0, 0, 16, 32, Tag, 0},
                 {Scratch, 0, 0, 0, 32, Tag, 1},
                 {Scratch, 0, 0, 0, 7, Tag, 0},
                 {Scratch, 0, 0, 0, 32, 0, 0}}) {
    auto V = Model->call("ExInitializeNPagedLookasideList", A);
    ASSERT_FALSE(bool(V));
    llvm::consumeError(V.takeError());
    EXPECT_EQ(bytes(Scratch), Before);
  }
  EXPECT_FALSE(failure("ExInitializeNPagedLookasideList",
                       {Scratch + 1, 0, 0, 0, 32, Tag, 0})
                   .empty());
  EXPECT_EQ(bytes(Scratch), Before);
  success(Memory->protect(Scratch, 0x1000, Read));
  EXPECT_NE(
      failure("ExInitializeNPagedLookasideList", {Scratch, 0, 0, 0, 32, Tag, 0})
          .find("writable"),
      std::string::npos);
  success(Memory->protect(Scratch, 0x1000, Read | Write));
  init(Scratch);
  const auto Initialized = bytes(Scratch);
  EXPECT_NE(
      failure("ExInitializeNPagedLookasideList", {Scratch, 0, 0, 0, 32, Tag, 0})
          .find("already initialized"),
      std::string::npos);
  EXPECT_NE(failure("ExInitializeNPagedLookasideList",
                    {Scratch + 16, 0, 0, 0, 32, Tag, 0})
                .find("kernel-owned"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Initialized);
  EXPECT_NE(
      failure("ExDeletePagedLookasideList", {Scratch}).find("matching kind"),
      std::string::npos);
  erase(Scratch);
  EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch}).find("live list"),
            std::string::npos);
}

TEST_F(KernelLookaside, CorruptedMetadataAndCacheAreRejectedBeforeRetirement) {
  init(Scratch);
  const uint64_t A = pool(), B = pool();
  call("ExpInterlockedPushEntrySList", {Scratch, A});
  call("ExpInterlockedPushEntrySList", {Scratch, B});
  for (auto [Offset, Width] : {std::pair{16u, 2u},
                               {18u, 2u},
                               {36u, 4u},
                               {40u, 4u},
                               {44u, 4u},
                               {48u, 8u},
                               {56u, 8u}}) {
    const auto Value = read(Scratch + Offset, Width);
    write(Scratch + Offset, Value + 1, Width);
    const auto Before = bytes(Scratch);
    EXPECT_NE(
        failure("ExDeleteNPagedLookasideList", {Scratch}).find("metadata"),
        std::string::npos);
    EXPECT_EQ(bytes(Scratch), Before);
    write(Scratch + Offset, Value, Width);
  }
  write(A, B);
  EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch}).find("cyclic"),
            std::string::npos);
  write(A, 0);
  write(Scratch, (read(Scratch) & ~uint64_t(0xffff)) | 1);
  EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch}).find("depth"),
            std::string::npos);
  write(Scratch, (read(Scratch) & ~uint64_t(0xffff)) | 2);
  const uint64_t Head = read(Scratch + 64);
  write(Scratch + 64, Scratch + 64);
  EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch}).find("self-link"),
            std::string::npos);
  write(Scratch + 64, Head);
  erase(Scratch);
}

TEST_F(KernelLookaside, DeletePreflightsEveryEntryBeforeChangingAnyBytes) {
  Model->enterExecution(profile::StackBase);
  const uint64_t Header = pool(0, 96), A = pool(), B = pool();
  init(Header);
  call("ExpInterlockedPushEntrySList", {Header, A});
  call("ExpInterlockedPushEntrySList", {Header, B});
  EXPECT_NE(failure("ExFreePool", {Header}).find("live lookaside"),
            std::string::npos);
  call("KeInitializeSpinLock", {A + 16});
  const uint64_t Old = call("KeAcquireSpinLockRaiseToDpc", {A + 16});
  const auto Before = bytes(Header), First = bytes(B, 32), Last = bytes(A, 32);
  EXPECT_NE(
      failure("ExDeleteNPagedLookasideList", {Header}).find("held executive"),
      std::string::npos);
  EXPECT_EQ(bytes(Header), Before);
  EXPECT_EQ(bytes(B, 32), First);
  EXPECT_EQ(bytes(A, 32), Last);
  call("KeReleaseSpinLock", {A + 16, Old});
  erase(Header);
  call("ExFreePool", {Header});
}

TEST_F(KernelLookaside, ResidencyIRQLAndInlineWriteBoundaries) {
  Model->enterExecution(profile::StackBase);
  const uint64_t PagedStorage = pool(1, 96);
  EXPECT_NE(failure("ExInitializePagedLookasideList",
                    {PagedStorage, 0, 0, 0, 32, Tag, 0})
                .find("nonpaged"),
            std::string::npos);
  call("ExFreePool", {PagedStorage});
  init(Scratch);
  init(Scratch + 0x100, true);
  success(Model->validateGuestAccess(Scratch, 16, true));
  success(Model->validateGuestAccess(Scratch + 20, 16, true));
  for (uint64_t Offset : {16, 18, 36, 40, 44, 48, 56, 64, 80, 88})
    EXPECT_NE(denied(Scratch + Offset, 1, true).find("kernel-owned"),
              std::string::npos);
  const uint64_t Old = call("KfRaiseIrql", {2});
  EXPECT_NE(denied(Scratch + 0x100, 1).find("APC_LEVEL"), std::string::npos);
  EXPECT_FALSE(
      failure("ExDeletePagedLookasideList", {Scratch + 0x100}).empty());
  EXPECT_FALSE(failure("ExInitializePagedLookasideList",
                       {Scratch + 0x200, 0, 0, 0, 32, Tag, 0})
                   .empty());
  init(Scratch + 0x200);
  erase(Scratch + 0x200);
  erase(Scratch);
  call("KeLowerIrql", {Old});
  erase(Scratch + 0x100, true);
}

TEST_F(KernelLookaside, ReadOnlyOrPinnedCachedEntryPreventsPartialDeletion) {
  init(Scratch, false, 4096);
  const uint64_t A = pool(0, 4096), B = pool(0, 4096);
  call("ExpInterlockedPushEntrySList", {Scratch, A});
  call("ExpInterlockedPushEntrySList", {Scratch, B});
  const auto Header = bytes(Scratch), First = bytes(B, 4096);
  success(Memory->protect(A, 4096, Read));
  EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch}).find("writable"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Header);
  EXPECT_EQ(bytes(B, 4096), First);
  success(Memory->protect(A, 4096, Read | Write));
  const uint64_t MDL = call("IoAllocateMdl", {A, 4096, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 0, 1});
  EXPECT_FALSE(failure("ExDeleteNPagedLookasideList", {Scratch}).empty());
  EXPECT_EQ(bytes(Scratch), Header);
  EXPECT_EQ(bytes(B, 4096), First);
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  erase(Scratch);
}

TEST_F(KernelLookaside, ForeignAndMultiplyCachedEntriesCannotBeRetired) {
  init(Scratch);
  init(Scratch + 0x100);
  const uint64_t A = pool();
  call("ExpInterlockedPushEntrySList", {Scratch, A});
  call("ExpInterlockedPushEntrySList", {Scratch + 0x100, A});
  const auto Before = bytes(Scratch);
  EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch}).find("cached"),
            std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
  EXPECT_EQ(call("ExpInterlockedPopEntrySList", {Scratch + 0x100}), A);
  erase(Scratch);
  for (auto [Type, Size, PoolTag] :
       {std::tuple<uint64_t, uint64_t, uint64_t>{1, 32, Tag},
        {0, 16, Tag},
        {0, 32, Tag + 1}}) {
    const uint64_t Foreign =
        call("ExAllocatePoolWithTag", {Type, Size, PoolTag});
    call("ExpInterlockedPushEntrySList", {Scratch + 0x100, Foreign});
    EXPECT_NE(failure("ExDeleteNPagedLookasideList", {Scratch + 0x100})
                  .find("matching live pool"),
              std::string::npos);
    EXPECT_EQ(call("ExpInterlockedPopEntrySList", {Scratch + 0x100}), Foreign);
    call("ExFreePool", {Foreign});
  }
  erase(Scratch + 0x100);
}

TEST_F(KernelLookaside, MDLBorrowsCannotBypassOwnedFieldsInEitherOrder) {
  const uint64_t Header = pool(0, 4096);
  uint64_t MDL = call("IoAllocateMdl", {Header, 96, 0, 0, 0});
  call("MmProbeAndLockPages", {MDL, 0, 1});
  EXPECT_NE(
      failure("ExInitializeNPagedLookasideList", {Header, 0, 0, 0, 32, Tag, 0})
          .find("borrow"),
      std::string::npos);
  call("MmUnlockPages", {MDL});
  call("IoFreeMdl", {MDL});
  MDL = call("IoAllocateMdl", {Header, 96, 0, 0, 0});
  call("MmBuildMdlForNonPagedPool", {MDL});
  EXPECT_NE(
      failure("ExInitializeNPagedLookasideList", {Header, 0, 0, 0, 32, Tag, 0})
          .find("borrow"),
      std::string::npos);
  call("IoFreeMdl", {MDL});
  init(Header);
  MDL = call("IoAllocateMdl", {Header, 96, 0, 0, 0});
  EXPECT_NE(failure("MmProbeAndLockPages", {MDL, 0, 1}).find("live lookaside"),
            std::string::npos);
  EXPECT_NE(failure("MmBuildMdlForNonPagedPool", {MDL}).find("live lookaside"),
            std::string::npos);
  erase(Header);
  call("MmBuildMdlForNonPagedPool", {MDL});
  call("IoFreeMdl", {MDL});
  call("ExFreePool", {Header});
}

TEST_F(KernelLookaside, LiveListsBlockUnloadAndRetainRecoveryDependency) {
  success(Model->captureUnpackBaseline());
  init(Scratch);
  auto Reasons = Model->unpackDependencies();
  ASSERT_TRUE(bool(Reasons)) << llvm::toString(Reasons.takeError());
  EXPECT_NE(std::find(Reasons->begin(), Reasons->end(),
                      "live kernel lookaside lists"),
            Reasons->end());
  write(Model->driverObject() + 0x68, 0x180001000);
  auto Unload = Model->beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("live lookaside"),
            std::string::npos);
  erase(Scratch);
  success(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(KernelLookaside, MissingDefaultCallbacksFailBeforeInitialization) {
  DriverOptions Options;
  Options.KernelExports["ExFreePool"] = false;
  initialize(Options);
  const auto Before = bytes(Scratch);
  EXPECT_NE(
      failure("ExInitializeNPagedLookasideList", {Scratch, 0, 0, 0, 32, Tag, 0})
          .find("unavailable"),
      std::string::npos);
  EXPECT_EQ(bytes(Scratch), Before);
}

} // namespace
} // namespace neverd::emulation
