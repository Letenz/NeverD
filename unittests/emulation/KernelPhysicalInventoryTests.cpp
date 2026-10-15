//===- KernelPhysicalInventoryTests.cpp - Declared RAM query boundaries
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declared RAM query boundaries.
///
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
constexpr uint64_t Base = 0x180000000, Scratch = 0x60000000, Page = 4096;
using Inventory = DriverPhysicalMemoryInventory;
Inventory declaration() {
  return {1, Inventory::SourceKind::ModelIdentityWindow,
          "independent inventory"};
}
void checked(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}
template <class T> T checked(llvm::Expected<T> V) {
  if (!V) {
    ADD_FAILURE() << llvm::toString(V.takeError());
    return {};
  }
  return std::move(*V);
}
void denied(llvm::Error E, llvm::StringRef Fragment) {
  ASSERT_TRUE(bool(E));
  const auto Text = llvm::toString(std::move(E));
  EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
}
template <class T> void denied(llvm::Expected<T> V, llvm::StringRef Fragment) {
  ASSERT_FALSE(bool(V));
  denied(V.takeError(), Fragment);
}
struct Session {
  static constexpr uint64_t Limit = 16 * 1024 * 1024;
  std::shared_ptr<AddressSpace> Memory = checked(
      AddressSpace::create(checked(PhysicalMemory::create(Limit)), Limit));
  DriverResult Result;
  KernelModel Model{*Memory, Result};
  DriverImage Image;
  Session() {
    checked(Memory->map(Base, 2 * Page, Read | Write));
    checked(Memory->protect(Base, Page, Read | Execute));
    checked(Memory->map(Scratch, Page, Read | Write));
    Image.Base = Base;
    Image.Size = 2 * Page;
    Image.Entry = Base;
    Image.Regions = {{Base, Read | Execute, std::vector<uint8_t>(Page)},
                     {Base + Page, Read | Write, std::vector<uint8_t>(Page)}};
  }
  void initialize(std::optional<Inventory> Value = declaration()) {
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.PhysicalMemoryInventory = std::move(Value);
    checked(Model.initialize(Image, Options));
  }
  uint64_t query() {
    return checked(Model.call("MmGetPhysicalMemoryRanges", {}));
  }
  std::vector<uint8_t> bytes(uint64_t Address, size_t Count = 32) {
    std::vector<uint8_t> B(Count);
    checked(Memory->snapshotBacking(Address, B));
    return B;
  }
};

// Independent little-endian oracle: one 64 MiB span at 64 GiB, then the
// complete zero/zero terminator. NumberOfBytes is not a page count.
const std::vector<uint8_t> Expected = {0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0,
                                       4, 0, 0, 0, 0,    0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0,    0, 0, 0, 0, 0};

TEST(KernelPhysicalInventory, ExactByteRangesAndTerminatorInOwnedPool) {
  Session S;
  S.initialize();
  const auto Buffer = S.query();
  ASSERT_NE(Buffer, 0u);
  EXPECT_EQ(Buffer % 16, 0u);
  EXPECT_EQ(S.bytes(Buffer), Expected);
  EXPECT_EQ(S.Model.unpackAllocations().at(Buffer), 32u);
  checked(S.Model.validateGuestAccess(Buffer, 32, false));
  checked(S.Model.validateGuestAccess(Buffer, 32, true));
  EXPECT_EQ(checked(S.Model.call("MmIsAddressValid", {Buffer})), 1u);
  EXPECT_FALSE(S.Model.takeGuestCall());
  EXPECT_EQ(S.Model.currentIRQL(), 0u);
  const auto MDL =
      checked(S.Model.call("IoAllocateMdl", {Buffer, 32, 0, 0, 0}));
  ASSERT_NE(MDL, 0u);
  checked(S.Model.call("MmBuildMdlForNonPagedPool", {MDL}));
  EXPECT_EQ(checked(S.Model.call("MmGetSystemAddressForMdlSafe",
                                 {MDL, windows::NormalPagePriority})),
            Buffer);
  checked(S.Model.call("IoFreeMdl", {MDL}));
  EXPECT_EQ(S.bytes(Buffer), Expected);
}

TEST(KernelPhysicalInventory, LiveResultsAreIndependentAndFreeExactlyOnce) {
  Session S;
  S.initialize();
  const auto A = S.query(), B = S.query();
  ASSERT_NE(A, 0u);
  ASSERT_NE(B, 0u);
  EXPECT_TRUE(A + 32 <= B || B + 32 <= A);
  checked(S.Memory->writeInteger(A, 0x12345678, 8));
  EXPECT_EQ(S.bytes(B), Expected);
  denied(S.Model.call("ExFreePoolWithTag", {A, 1}), "tag");
  EXPECT_EQ(checked(S.Memory->readInteger(A, 8)), 0x12345678u);
  checked(S.Model.call("ExFreePoolWithTag", {A, 0}));
  EXPECT_EQ(S.bytes(B), Expected);
  denied(S.Model.validateGuestAccess(A, 1, false), "freed");
  denied(S.Model.call("ExFreePool", {A}), "already freed");
  const auto C = S.query();
  ASSERT_NE(C, 0u);
  EXPECT_NE(C, B);
  EXPECT_EQ(S.bytes(C), Expected);
  checked(S.Model.call("ExFreePool", {B}));
  EXPECT_EQ(S.bytes(C), Expected);
  checked(S.Model.call("ExFreePool", {C}));
}

TEST(KernelPhysicalInventory, AdmissionFailureDoesNotAllocateOrChangeBytes) {
  for (bool Declared : {false, true}) {
    Session S;
    S.initialize(Declared ? std::optional<Inventory>(declaration())
                          : std::nullopt);
    const auto Before = S.Model.unpackAllocations();
    const auto Original = S.bytes(Scratch);
    denied(S.Model.call("MmGetPhysicalMemoryRanges", {0}), "argument count");
    if (!Declared)
      denied(S.Model.call("MmGetPhysicalMemoryRanges", {}), "not declared");
    else {
      S.Model.enterExecution(profile::StackBase);
      EXPECT_EQ(checked(S.Model.call("KfRaiseIrql", {1})), 0u);
      denied(S.Model.call("MmGetPhysicalMemoryRanges", {}), "PASSIVE_LEVEL");
      checked(S.Model.call("KeLowerIrql", {0}));
      checked(S.Model.validateExecutionReturn(profile::StackBase, 0));
    }
    EXPECT_EQ(S.Model.unpackAllocations(), Before);
    EXPECT_EQ(S.bytes(Scratch), Original);
  }
}

TEST(KernelPhysicalInventory, InvalidDeclarationFailsBeforeKernelAllocation) {
  auto Invalid = std::vector<Inventory>(7, declaration());
  Invalid[0].Version = 0;
  Invalid[1].Version = 2;
  Invalid[2].Source.reset();
  Invalid[3].Source = static_cast<Inventory::SourceKind>(77);
  Invalid[4].Evidence.clear();
  Invalid[5].Evidence = std::string(257, 'x');
  Invalid[6].Evidence = "bad\nlabel";
  for (const auto &Value : Invalid) {
    Session S;
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.PhysicalMemoryInventory = Value;
    denied(S.Model.initialize(S.Image, Options), "physical_memory_inventory:");
    EXPECT_EQ(S.Model.driverObject(), 0u);
    EXPECT_TRUE(S.Model.unpackAllocations().empty());
    S.initialize();
    EXPECT_NE(S.query(), 0u);
  }
  Session S;
  DriverOptions Legacy;
  Legacy.PhysicalMemoryInventory = declaration();
  denied(S.Model.initialize(S.Image, Legacy), "checked-x64");
  EXPECT_TRUE(S.Model.unpackAllocations().empty());
}

TEST(KernelPhysicalInventory,
     CopiedDeclarationRetainsOpaqueLifetimeDependencies) {
  Session S;
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.PhysicalMemoryInventory = declaration();
  checked(S.Model.initialize(S.Image, Options));
  Options.PhysicalMemoryInventory->Source.reset();
  checked(S.Model.captureUnpackBaseline());
  const auto Baseline = checked(S.Model.unpackDependencies());
  EXPECT_EQ(Baseline, std::vector<std::string>(
                          {"unreconstructed kernel or environment effects"}));
  const auto Buffer = S.query();
  ASSERT_NE(Buffer, 0u);
  const auto Ordinary = checked(S.Model.call("ExAllocatePool", {0, 64}));
  ASSERT_NE(Ordinary, 0u);
  for (bool Backing : {false, true}) {
    const auto State = checked(S.Model.captureUnpackRuntime(Backing));
    ASSERT_EQ(State.Pools.size(), 1u);
    EXPECT_EQ(State.Pools[0].Address, Ordinary);
    EXPECT_EQ(State.Pools[0].API, WindowsDriverState::PoolAPI::ExAllocatePool);
    EXPECT_EQ(State.Pools[0].Bytes.has_value(), Backing);
    for (const auto *Reason :
         {"live kernel pool allocations", "retained kernel object allocations",
          "unreconstructed kernel or environment effects"})
      EXPECT_NE(std::find(State.Reasons.begin(), State.Reasons.end(), Reason),
                State.Reasons.end());
    EXPECT_EQ(S.Model.unpackAllocations().at(Buffer), 32u);
  }
  checked(S.Model.call("ExFreePool", {Buffer}));
  EXPECT_EQ(S.Model.unpackAllocations().count(Buffer), 0u);
  checked(S.Model.call("ExFreePool", {Ordinary}));
  EXPECT_EQ(checked(S.Model.unpackDependencies()), Baseline);
  EXPECT_TRUE(checked(S.Model.captureUnpackRuntime(true)).Pools.empty());
}

TEST(KernelPhysicalInventory, PinnedResultCannotBeFreedAndIdentityIsInRange) {
  Session S;
  S.initialize();
  const auto Buffer = S.query();
  ASSERT_NE(Buffer, 0u);
  const auto MDL =
      checked(S.Model.call("IoAllocateMdl", {Buffer, 32, 0, 0, 0}));
  ASSERT_NE(MDL, 0u);
  checked(S.Model.call("MmProbeAndLockPages",
                       {MDL, windows::KernelMode, windows::IoReadAccess}));
  const auto PFN = checked(S.Memory->readInteger(MDL + windows::MDLSize, 8));
  EXPECT_GE(PFN * Page, 0x1000000000u);
  EXPECT_LT(PFN * Page, 0x1004000000u);
  const auto Before = S.Model.unpackAllocations();
  denied(S.Model.call("ExFreePool", {Buffer}), "pinned");
  EXPECT_EQ(S.Model.unpackAllocations(), Before);
  EXPECT_EQ(S.bytes(Buffer), Expected);
  checked(S.Model.call("MmUnlockPages", {MDL}));
  checked(S.Model.call("IoFreeMdl", {MDL}));
  checked(S.Model.call("ExFreePool", {Buffer}));
  denied(S.Model.validateGuestAccess(Buffer, 32, false), "freed");
}

TEST(KernelPhysicalInventory,
     ShortageReturnsNullWithoutPublishingAnotherObject) {
  for (uint64_t Chunk : {65536u, 16u}) {
    Session S;
    S.initialize();
    const auto Retained = S.query();
    ASSERT_NE(Retained, 0u);
    uint64_t Last = Retained;
    unsigned Count = 0;
    for (; Count < 4096; ++Count) {
      const auto Next = checked(S.Model.call("ExAllocatePool", {0, Chunk}));
      if (!Next)
        break;
      Last = Next;
    }
    ASSERT_LT(Count, 4096u);
    // Tiny allocations exhaust physical owner identities while most arena
    // space remains. Large allocations independently exhaust arena capacity.
    if (Chunk == 16)
      EXPECT_LT(Last - Retained, profile::KernelArenaSize / 2);
    bool Exhausted = false;
    for (unsigned I = 0; I < 4096; ++I) {
      const auto Before = S.Model.unpackAllocations();
      if (!S.query()) {
        EXPECT_EQ(S.Model.unpackAllocations(), Before);
        EXPECT_EQ(S.bytes(Retained), Expected);
        Exhausted = true;
        break;
      }
    }
    EXPECT_TRUE(Exhausted) << "bounded pool never reported shortage";
  }
}

TEST(KernelPhysicalInventory,
     InventoryDoesNotGrantPhysicalReadOrMappingAuthority) {
  Session S;
  S.initialize();
  const auto Buffer = S.query();
  ASSERT_NE(Buffer, 0u);
  // A high unassigned page is a declared RAM location, not known guest bytes.
  EXPECT_FALSE(checked(S.Memory->canAccess(0x1003fff000, Page, Read)));
  denied(S.Model.call("MmMapIoSpace", {0x1003fff000, Page, 0}),
         "not one declared translated resource");
  EXPECT_FALSE(KernelModel::argumentCount("MmGetPhysicalMemoryRangesEx"));
  EXPECT_FALSE(KernelModel::argumentCount("MmGetPhysicalMemoryRangesEx2"));
  const auto MDL =
      checked(S.Model.call("IoAllocateMdl", {Base + Page, Page, 0, 0, 0}));
  ASSERT_NE(MDL, 0u);
  checked(S.Model.call("MmProbeAndLockPages",
                       {MDL, windows::KernelMode, windows::IoReadAccess}));
  const auto PFN = checked(S.Memory->readInteger(MDL + windows::MDLSize, 8));
  EXPECT_GE(PFN * Page, 0x1000000000u);
  EXPECT_LT(PFN * Page, 0x1004000000u);
  EXPECT_FALSE(checked(S.Memory->canAccess(0x1003fff000, Page, Read)));
  checked(S.Model.call("MmUnlockPages", {MDL}));
  checked(S.Model.call("IoFreeMdl", {MDL}));
}
} // namespace
} // namespace neverd::emulation
