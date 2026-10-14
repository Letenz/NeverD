//===- KernelResidentQueryTests.cpp - Bounded kernel residency queries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include <array>

namespace neverd::emulation {
namespace {
constexpr uint64_t Scratch = 0x60000000;
constexpr uint64_t Page = 4096;
constexpr uint64_t MemoryLimit = 16 * 1024 * 1024;
constexpr uint32_t Tag = 0x71756572;

void checked(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}
template <class T> T checked(llvm::Expected<T> Value) {
  if (!Value) {
    ADD_FAILURE() << llvm::toString(Value.takeError());
    return {};
  }
  return std::move(*Value);
}

class KernelResidentQuery : public testing::Test {
protected:
  std::shared_ptr<AddressSpace> Memory = checked(AddressSpace::create(
      checked(PhysicalMemory::create(MemoryLimit)), MemoryLimit));
  DriverResult Result;
  KernelModel Model{*Memory, Result};
  DriverImage Image;
  void SetUp() override {
    checked(Memory->map(Scratch, Page, Read | Write));
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + Page;
    Image.Size = 3 * Page;
    checked(Memory->map(Image.Base, Image.Size, Read | Execute));
    checked(Model.initialize(Image, DriverOptions{}));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return checked(Model.call(Name, Arguments));
  }
  uint64_t query(uint64_t Address) {
    return call("MmIsAddressValid", {Address});
  }
  void rejects(uint64_t Address, llvm::StringRef Fragment) {
    auto Value = Model.call("MmIsAddressValid", {Address});
    ASSERT_FALSE(bool(Value));
    const auto Text = llvm::toString(Value.takeError());
    EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
  }
};

TEST_F(KernelResidentQuery, NullRequiresAnActuallyUnmappedZeroPage) {
  EXPECT_EQ(query(0), 0u);
  for (unsigned Permissions : {0u, unsigned(Read), unsigned(Read | Write)}) {
    checked(Memory->map(0, Page, Permissions));
    rejects(0, "mapped zero");
    checked(Memory->unmap(0, Page));
  }
  unsigned Effects = 0;
  GuestMMIOCallbacks IO;
  IO.Validate = [&](uint64_t, uint64_t, bool) {
    ++Effects;
    return llvm::Error::success();
  };
  IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Effects;
    return 0;
  };
  IO.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Effects;
    return llvm::Error::success();
  };
  checked(Memory->mapMMIO(0, Page, std::move(IO)));
  rejects(0, "mapped zero");
  EXPECT_EQ(Effects, 0u);
  checked(Memory->unmapMMIO(0, Page));
  EXPECT_EQ(query(0), 0u);
}

TEST_F(KernelResidentQuery, ResidencyDoesNotExposeOpaqueObjectFields) {
  const auto Object = Model.driverObject();
  std::array<uint8_t, 336> Before{}, After{};
  checked(Memory->snapshotBacking(Object, Before));
  EXPECT_EQ(query(Object), 1u);
  EXPECT_EQ(query(Object + 335), 1u);
  EXPECT_EQ(query(Object + 16), 1u);
  for (bool Write : {false, true}) {
    auto E = Model.validateGuestAccess(Object + 16, 4, Write);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  checked(Memory->snapshotBacking(Object, After));
  EXPECT_EQ(After, Before);
}

TEST_F(KernelResidentQuery, ExactLivePoolBytesDoNotReviveFreedNeighbors) {
  const auto First = call("ExAllocatePoolWithTag", {512, 17, Tag});
  const auto Next = call("ExAllocatePool2", {0x40, 17, Tag});
  ASSERT_NE(First, 0u);
  ASSERT_NE(Next, 0u);
  ASSERT_EQ(First / Page, Next / Page);
  ASSERT_GT(Next, First + 17);
  for (uint64_t Base : {First, Next}) {
    EXPECT_EQ(query(Base), 1u);
    EXPECT_EQ(query(Base + 16), 1u);
    rejects(Base + 17, "without padding");
  }
  const auto Initial = checked(Memory->readInteger(First, 8));
  EXPECT_EQ(query(First + 8), 1u);
  EXPECT_EQ(checked(Memory->readInteger(First, 8)), Initial);
  call("ExFreePoolWithTag", {First, Tag});
  rejects(First, "retired");
  rejects(First + 16, "retired");
  EXPECT_EQ(query(Next), 1u);
  EXPECT_EQ(query(Next + 16), 1u);
}

TEST_F(KernelResidentQuery, PagedPoolAndUnownedMappingsHaveNoGuessedResult) {
  const auto Paged = call("ExAllocatePoolWithTag", {1, 17, Tag});
  ASSERT_NE(Paged, 0u);
  rejects(Paged, "paged pool");
  rejects(Paged + 16, "paged pool");
  for (uint64_t Address :
       {Scratch, Image.Base, Image.Entry, uint64_t(1), UINT64_MAX})
    rejects(Address, "live modeled kernel");
  const auto Pool = call("ExAllocatePool", {0, 32});
  checked(
      Memory->mapAlias(Scratch + Page, Pool & ~(Page - 1), Page, Read | Write));
  rejects(Scratch + Page + (Pool & (Page - 1)), "live modeled kernel");
  EXPECT_EQ(query(Pool), 1u);
}

TEST_F(KernelResidentQuery, MissingOrProtectedBackingCannotReturnTrue) {
  const auto Pool = call("ExAllocatePool2", {0x40, Page, Tag});
  ASSERT_NE(Pool, 0u);
  ASSERT_EQ(Pool % Page, 0u);
  for (unsigned Permissions : {0u, unsigned(Read), unsigned(Write)}) {
    checked(Memory->protect(Pool, Page, Permissions));
    rejects(Pool, "readable and writable RAM");
    rejects(Pool + Page - 1, "readable and writable RAM");
  }
  checked(Memory->protect(Pool, Page, Read | Write));
  EXPECT_EQ(query(Pool), 1u);
  checked(Memory->unmap(Pool, Page));
  rejects(Pool, "readable and writable RAM");
}

TEST_F(KernelResidentQuery, DispatchCeilingIsCheckedBeforeTheNullResult) {
  Model.enterExecution(profile::StackBase);
  EXPECT_EQ(call("KfRaiseIrql", {2}), 0u);
  EXPECT_EQ(query(Model.driverObject()), 1u);
  EXPECT_EQ(query(0), 0u);
  EXPECT_EQ(call("KfRaiseIrql", {3}), 2u);
  rejects(0, "IRQL");
  rejects(Model.driverObject(), "IRQL");
  EXPECT_EQ(Model.currentIRQL(), 3u);
  call("KeLowerIrql", {2});
  call("KeLowerIrql", {0});
  checked(Model.validateExecutionReturn(profile::StackBase, 0));
  EXPECT_EQ(query(0), 0u);
}

TEST_F(KernelResidentQuery, QueryBeforeOrAfterBaselineRetainsDependency) {
  checked(Model.captureUnpackBaseline());
  EXPECT_TRUE(checked(Model.unpackDependencies()).empty());
  for (bool BeforeBaseline : {true, false}) {
    auto Space = checked(AddressSpace::create(
        checked(PhysicalMemory::create(MemoryLimit)), MemoryLimit));
    DriverResult Report;
    KernelModel Other(*Space, Report);
    checked(Other.initialize(Image, DriverOptions{}));
    if (!BeforeBaseline)
      checked(Other.captureUnpackBaseline());
    EXPECT_EQ(checked(Other.call("MmIsAddressValid", {0})), 0u);
    if (BeforeBaseline)
      checked(Other.captureUnpackBaseline());
    auto State = checked(Other.captureUnpackRuntime(true));
    EXPECT_TRUE(State.hasAdditionalDependencies());
    EXPECT_TRUE(State.Pools.empty());
    EXPECT_EQ(State.Reasons,
              (std::vector<std::string>{
                  "unreconstructed kernel or environment effects"}));
  }
}
} // namespace
} // namespace neverd::emulation
