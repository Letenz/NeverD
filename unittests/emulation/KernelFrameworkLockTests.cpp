//===- KernelFrameworkLockTests.cpp - WDF lock execution and lifetime -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelFramework.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace framework;
using namespace windows;
class FrameworkLockTest : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Info = Scratch, Component = Scratch + 0x100;
  static constexpr uint64_t TableSlot = Scratch + 0x180;
  static constexpr uint64_t GlobalsSlot = Scratch + 0x188;
  static constexpr uint64_t Config = Scratch + 0x200;
  static constexpr uint64_t Output = Scratch + 0x280;
  static constexpr uint64_t Timeout = Scratch + 0x300;
  static constexpr uint64_t ParentAttrs = Scratch + 0x400;
  static constexpr uint64_t ThreadA = 0xa0, ThreadB = 0xb0;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;
  uint64_t Globals = 0;

  void ok(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  template <typename T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    EXPECT_NE(llvm::toString(Value.takeError()).find(Text.str()),
              std::string::npos);
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t get(uint64_t Address) {
    return take(Memory->readInteger(Address, 8));
  }
  llvm::Expected<uint64_t> invoke(llvm::StringRef Name,
                                  std::initializer_list<uint64_t> Arguments) {
    auto Address = Exports.insertFrameworkFunction(Globals, Name);
    if (!Address)
      return Address.takeError();
    return Model->call(*Exports.lookup(*Address), Arguments, nullptr);
  }
  void thread(uint64_t Thread, uint64_t Frame = 0) {
    Model->enterForeground();
    Model->enterExecution(Frame ? Frame : Thread, Thread);
  }
  uint64_t create(bool Waitable, uint64_t Attributes = 0) {
    EXPECT_EQ(
        take(invoke(Waitable ? api::WdfWaitLockCreate : api::WdfSpinLockCreate,
                    {Globals, Attributes, Output})),
        StatusSuccess);
    return get(Output);
  }
  bool apcsDisabled() { return take(Model->call("KeAreApcsDisabled", {})); }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    ok(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    thread(ThreadA);
    constexpr llvm::StringLiteral Name = "KmdfLibrary";
    for (size_t I = 0; I < Name.size(); ++I)
      put(Component + I * 2, Name[I], 2);
    put(Info, BindV1Size, 4);
    put(Info + BindComponent, Component);
    put(Info + BindMajor, MajorVersion, 4);
    put(Info + BindMinor, MinorVersion, 4);
    put(Info + BindCount, FunctionCount, 4);
    put(Info + BindTable, TableSlot);
    const auto Bind =
        take(Exports.bindImport({0, "WDFLDR.SYS", "WdfVersionBind"}));
    EXPECT_EQ(take(Model->call(*Exports.lookup(Bind),
                               {Model->driverObject(), Model->registryPath(),
                                Info, GlobalsSlot},
                               nullptr)),
              StatusSuccess);
    Globals = get(GlobalsSlot);
    put(Config, DriverConfigSize, 4);
    put(Config + DriverConfigFlags, DriverNonPnp, 4);
    put(Config + DriverConfigUnload, Image.Entry);
    EXPECT_EQ(take(invoke(api::WdfDriverCreate,
                          {Globals, Model->driverObject(),
                           Model->registryPath(), 0, Config, Output})),
              StatusSuccess);
  }
};

TEST_F(FrameworkLockTest, SpinLockSharesExecutiveOwnershipAndRestoresIRQL) {
  const auto Lock = create(false);
  take(invoke(api::WdfSpinLockAcquire, {Globals, Lock}));
  EXPECT_EQ(Model->currentIRQL(), scheduler::DispatchLevel);
  reject(invoke(api::WdfSpinLockAcquire, {Globals, Lock}), "deadlock");
  take(invoke(api::WdfSpinLockRelease, {Globals, Lock}));
  EXPECT_EQ(Model->currentIRQL(), scheduler::PassiveLevel);
  reject(invoke(api::WdfSpinLockRelease, {Globals, Lock}), "not been acquired");
  take(invoke(api::WdfObjectDelete, {Globals, Lock}));
  reject(invoke(api::WdfSpinLockAcquire, {Globals, Lock}), "live matching");
}

TEST_F(FrameworkLockTest, WaitLockRetainsWaitersAndUsesThreadAPCState) {
  const auto Lock = create(true);
  take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}));
  EXPECT_TRUE(apcsDisabled());
  reject(invoke(api::WdfObjectDelete, {Globals, Lock}), "owned wait lock");
  thread(ThreadB);
  put(Timeout, 0);
  EXPECT_EQ(take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, Timeout})),
            StatusTimeout);
  EXPECT_FALSE(apcsDisabled());
  take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}));
  const auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  EXPECT_TRUE(apcsDisabled());
  EXPECT_FALSE(take(Model->pollWait(*Pending)));
  thread(ThreadA);
  take(invoke(api::WdfWaitLockRelease, {Globals, Lock}));
  EXPECT_FALSE(apcsDisabled());
  reject(invoke(api::WdfObjectDelete, {Globals, Lock}), "outstanding waits");
  EXPECT_EQ(take(Model->pollWait(*Pending)), StatusSuccess);
  thread(ThreadB, ThreadB + 1);
  EXPECT_TRUE(apcsDisabled());
  take(invoke(api::WdfWaitLockRelease, {Globals, Lock}));
  EXPECT_FALSE(apcsDisabled());
  take(invoke(api::WdfObjectDelete, {Globals, Lock}));
}

TEST_F(FrameworkLockTest, TimedWaitReleasesAPCRegionAndZeroPollEnforcesIRQL) {
  const auto Lock = create(true);
  take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}));
  thread(ThreadB);
  put(Timeout, uint64_t(-100));
  take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, Timeout}));
  const auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending && Pending->Deadline);
  EXPECT_FALSE(take(Model->nextScheduled(true, Pending->Deadline)));
  EXPECT_EQ(take(Model->pollWait(*Pending)), StatusTimeout);
  EXPECT_FALSE(apcsDisabled());
  put(Timeout, 0);
  const auto Old = take(Model->call("KfRaiseIrql", {scheduler::DispatchLevel}));
  reject(invoke(api::WdfWaitLockAcquire, {Globals, Lock, Timeout}),
         "below DISPATCH_LEVEL");
  take(Model->call("KeLowerIrql", {Old}));
  thread(ThreadA);
  take(invoke(api::WdfWaitLockRelease, {Globals, Lock}));
  take(invoke(api::WdfObjectDelete, {Globals, Lock}));
}

TEST_F(FrameworkLockTest, ParentDeletionWaitsForTheOwnedChildLock) {
  EXPECT_EQ(take(invoke(api::WdfObjectCreate, {Globals, 0, Output})), 0u);
  const auto Parent = get(Output);
  put(ParentAttrs, AttributesSize, 4);
  put(ParentAttrs + AttributesExecution, ExecutionInherit, 4);
  put(ParentAttrs + AttributesSynchronization, SynchronizationInherit, 4);
  put(ParentAttrs + AttributesParent, Parent);
  const auto Lock = create(true, ParentAttrs);
  take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}));
  reject(invoke(api::WdfObjectDelete, {Globals, Parent}), "owned wait lock");
  take(invoke(api::WdfWaitLockRelease, {Globals, Lock}));
  take(invoke(api::WdfObjectDelete, {Globals, Parent}));
  reject(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}), "live matching");
}

TEST_F(FrameworkLockTest, NestedExecutionCannotReturnWithAnAcquiredWaitLock) {
  const auto Lock = create(true);
  constexpr uint64_t NestedFrame = ThreadA + 1;
  thread(ThreadA, NestedFrame);
  take(invoke(api::WdfWaitLockAcquire, {Globals, Lock, 0}));
  auto Error = Model->validateExecutionReturn(NestedFrame, 0, true);
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("framework wait lock"),
            std::string::npos);
  take(invoke(api::WdfWaitLockRelease, {Globals, Lock}));
  ok(Model->validateExecutionReturn(NestedFrame, 0, true));
  take(invoke(api::WdfObjectDelete, {Globals, Lock}));
}
} // namespace
} // namespace neverd::emulation
