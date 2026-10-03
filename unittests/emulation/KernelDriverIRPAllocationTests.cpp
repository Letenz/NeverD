//===- KernelDriverIRPAllocationTests.cpp - Caller IRP storage ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate allocator ABI, exact storage ownership and failed-free atomicity
/// without inventing an I/O request before the driver submits its packet.
///
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelDriverIRPAllocation : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint32_t PoolTag = 0x7052494e;
  std::unique_ptr<UnicornBackend> Memory;
  std::unique_ptr<KernelModel> Model;
  DriverResult Result;

  void success(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void rejected(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
  }
  template <class T>
  void rejected(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    rejected(Value.takeError(), Text);
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    success(Memory->map(Scratch, profile::PageSize, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = Entry - profile::PageSize;
    Image.Entry = Entry;
    Image.Size = 3 * profile::PageSize;
    success(Model->initialize(Image, DriverOptions{}));
    Model->enterForeground();
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef API,
                std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(API.str(), Arguments));
  }
  uint64_t get(uint64_t Address, unsigned Size = sizeof(uint64_t)) {
    success(Model->validateGuestAccess(Address, Size, false));
    return take(Memory->readInteger(Address, Size));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Size = sizeof(uint64_t)) {
    success(Model->validateGuestAccess(Address, Size, true));
    success(Memory->writeInteger(Address, Value, Size));
  }
  uint64_t allocate(uint8_t Count = 1) {
    return call("IoAllocateIrp", {Count, false});
  }
  void free(uint64_t IRP) { call("IoFreeIrp", {IRP}); }
};

TEST_F(KernelDriverIRPAllocation,
       FreshHeaderUsesOnePastStackWithoutRequestRow) {
  for (uint8_t Count : {uint8_t(1), uint8_t(2), uint8_t(MaxIRPStackCount)}) {
    const auto IRP = allocate(Count);
    ASSERT_NE(IRP, 0u);
    const uint64_t Size = IRPSize + uint64_t(Count) * StackSize;
    EXPECT_EQ(get(IRP + IRPTypeOffset, 2), IRPType);
    EXPECT_EQ(get(IRP + ObjectSizeOffset, 2), Size);
    EXPECT_EQ(get(IRP + IRPStackCountOffset, 1), Count);
    EXPECT_EQ(get(IRP + IRPLocationOffset, 1), uint32_t(Count) + 1);
    EXPECT_EQ(get(IRP + IRPStackPointerOffset), IRP + Size);
    EXPECT_EQ(get(IRP + IRPRequestorModeOffset, 1), KernelMode);
    EXPECT_EQ(get(IRP + IRPThreadOffset), 0u);
    EXPECT_EQ(get(IRP + IRPMdlOffset), 0u);
    EXPECT_EQ(get(IRP + IRPCancelRoutineOffset), 0u);
    std::vector<uint8_t> Stack(Count * StackSize);
    success(Memory->read(IRP + IRPSize, Stack));
    EXPECT_TRUE(std::all_of(Stack.begin(), Stack.end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_TRUE(Result.Requests.empty());
    free(IRP);
  }
  EXPECT_TRUE(Result.Requests.empty());
  EXPECT_TRUE(Result.Devices.empty());
}

TEST_F(KernelDriverIRPAllocation,
       RejectsNonpositiveStackAndQuotaBeforeAllocation) {
  for (uint64_t Count : {0u, 128u, 255u})
    rejected(Model->call("IoAllocateIrp", {Count, false}), "positive bounded");
  rejected(Model->call("IoAllocateIrp", {1, true}), "ChargeQuota");
  EXPECT_TRUE(Result.Requests.empty());
  const auto IRP = allocate();
  ASSERT_NE(IRP, 0u);
  free(IRP);
}

TEST_F(KernelDriverIRPAllocation,
       FreeRequiresLiveExactBaseAndRevokesOnlyPacket) {
  const auto First = allocate();
  const auto Second = allocate();
  ASSERT_NE(First, 0u);
  ASSERT_NE(Second, 0u);
  for (uint64_t Foreign : {uint64_t(0), Scratch, First + 1, Second + IRPSize})
    rejected(Model->call("IoFreeIrp", {Foreign}), "exact base");
  EXPECT_EQ(get(First + IRPTypeOffset, 2), IRPType);
  free(First);
  rejected(Model->call("IoFreeIrp", {First}), "live caller-allocated");
  rejected(Model->validateGuestAccess(First, 1, false), "freed");
  rejected(Model->validateGuestAccess(First + IRPSize, StackSize, true),
           "freed");
  EXPECT_EQ(get(Second + IRPTypeOffset, 2), IRPType);
  free(Second);
  EXPECT_TRUE(Result.Requests.empty());
}

TEST_F(KernelDriverIRPAllocation, FreeDoesNotConsumeAttachedCallerMDLOrBuffer) {
  const auto IRP = allocate();
  const auto Pool = call("ExAllocatePoolWithTag", {0, 64, PoolTag});
  const auto MDL = call("IoAllocateMdl", {Pool, 64, false, false, 0});
  ASSERT_NE(MDL, 0u);
  call("MmBuildMdlForNonPagedPool", {MDL});
  put(IRP + IRPMdlOffset, MDL);
  free(IRP);
  EXPECT_EQ(get(MDL + MDLMappedSystemVAOffset), Pool);
  put(Pool, 0x1122334455667788);
  EXPECT_EQ(get(Pool), 0x1122334455667788u);
  call("IoFreeMdl", {MDL});
  call("ExFreePoolWithTag", {Pool, PoolTag});
  EXPECT_TRUE(Result.Requests.empty());
}

TEST_F(KernelDriverIRPAllocation, FailedFreePreservesPacketAndHeldLock) {
  const auto IRP = allocate();
  const auto Lock = IRP + IRPDriverContextOffset;
  call("KeInitializeSpinLock", {Lock});
  const auto Previous = call("KeAcquireSpinLockRaiseToDpc", {Lock});
  rejected(Model->call("IoFreeIrp", {IRP}), "held executive spin lock");
  EXPECT_EQ(get(IRP + IRPTypeOffset, 2), IRPType);
  call("KeReleaseSpinLock", {Lock, Previous});
  free(IRP);
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
}

TEST_F(KernelDriverIRPAllocation,
       DispatchLevelIsAllowedAndHigherIRQLIsRejected) {
  const auto Previous = call("KfRaiseIrql", {scheduler::DispatchLevel});
  const auto IRP = allocate();
  ASSERT_NE(IRP, 0u);
  EXPECT_EQ(get(IRP + IRPTypeOffset, 2), IRPType);
  const auto Dispatch = call("KfRaiseIrql", {scheduler::DispatchLevel + 1});
  rejected(Model->call("IoAllocateIrp", {1, false}), "IRQL");
  rejected(Model->call("IoFreeIrp", {IRP}), "IRQL");
  call("KeLowerIrql", {Dispatch});
  free(IRP);
  call("KeLowerIrql", {Previous});
}

TEST_F(KernelDriverIRPAllocation,
       ExhaustionReturnsNullWithoutPublishingRequest) {
  std::vector<uint64_t> IRPs;
  const size_t Bound = profile::KernelArenaSize / IRPSize + 1;
  while (IRPs.size() < Bound) {
    const auto IRP = allocate(MaxIRPStackCount);
    if (!IRP)
      break;
    IRPs.push_back(IRP);
  }
  ASSERT_LT(IRPs.size(), Bound);
  ASSERT_FALSE(IRPs.empty());
  EXPECT_EQ(allocate(MaxIRPStackCount), 0u);
  EXPECT_TRUE(Result.Requests.empty());
  for (uint64_t IRP : IRPs)
    free(IRP);
}

TEST_F(KernelDriverIRPAllocation,
       CallerAllocationIsNotBoundToAllocatingThread) {
  const auto IRP = allocate();
  Model->enterExecution(profile::CallbackStackBase);
  free(IRP);
  rejected(Model->validateGuestAccess(IRP, 1, false), "freed");
  EXPECT_TRUE(Result.Requests.empty());
}

} // namespace
} // namespace neverd::emulation
