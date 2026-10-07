//===- KernelWaitSetTests.cpp - Atomic dispatcher wait sets ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelAPINames.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/KernelWaits.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

#include <bit>

namespace neverd::emulation {
namespace {
#define NEVERD_MULTI_WAIT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "fixtures/DriverMultipleWaitCases.def"
#undef NEVERD_MULTI_WAIT_VALUE

class KernelMultipleWait : public testing::Test {
protected:
  std::shared_ptr<AddressSpace> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;

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
  template <typename T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text = {}) {
    ASSERT_FALSE(bool(Value));
    const auto Error = llvm::toString(Value.takeError());
    EXPECT_NE(Error.find(Text.str()), std::string::npos) << Error;
  }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name.str(), Arguments));
  }
  uint64_t pool(uint64_t Size, uint32_t Type = 0) {
    return call(kernel_api::ExAllocatePoolWithTag, {Type, Size, PoolTag});
  }
  void put(uint64_t Address, uint64_t Value) {
    ok(Memory->writeInteger(Address, Value, sizeof(uint64_t)));
  }
  uint64_t get(uint64_t Address) {
    return take(Memory->readInteger(Address, 8));
  }
  void array(llvm::ArrayRef<uint64_t> Objects) {
    for (size_t I = 0; I < Objects.size(); ++I)
      put(Scratch + I * sizeof(uint64_t), Objects[I]);
  }
  llvm::Expected<uint64_t> wait(llvm::ArrayRef<uint64_t> Objects, bool All,
                                uint64_t Timeout = 0, uint64_t Blocks = 0) {
    array(Objects);
    return Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                       {Objects.size(), Scratch, All ? 0u : 1u,
                        windows::ExecutiveWaitReason, windows::KernelMode,
                        false, Timeout, Blocks});
  }
  uint64_t event(bool Signaled = false, bool Notification = false) {
    const auto Object = pool(dispatcher::EventSize);
    call(kernel_api::KeInitializeEvent,
         {Object, Notification ? 0u : 1u, Signaled});
    return Object;
  }
  void signal(uint64_t Object) { call(kernel_api::KeSetEvent, {Object, 0, 0}); }
  void SetUp() override {
    Memory = take(AddressSpace::create(
        take(PhysicalMemory::create(MemoryLimit)), MemoryLimit));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, ScratchSize, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = ImageBase;
    Image.Entry = ImageBase + EntryOffset;
    Image.Size = ImageSize;
    DriverOptions Options;
    Options.Scheduling = DriverScheduling{SmallQuantum};
    ok(Model->initialize(Image, Options));
    Model->enterExecution(profile::StackBase);
  }
};

TEST_F(KernelMultipleWait, WaitAllLeavesPartialSignalsAndMutexUnacquired) {
  const auto Event = event(true);
  const auto Gate = event();
  const auto Mutex = pool(dispatcher::MutexSize);
  const auto Semaphore = pool(dispatcher::SemaphoreSize);
  call(kernel_api::KeInitializeMutex, {Mutex, 0});
  call(kernel_api::KeInitializeSemaphore, {Semaphore, 1, 1});
  const auto Blocks = pool(4 * WaitBlockBytes);
  EXPECT_EQ(take(wait({Event, Semaphore, Mutex, Gate}, true, 0, Blocks)), 0u);
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateMutex, {Mutex}), 1u);
  EXPECT_EQ(call(kernel_api::KeAreApcsDisabled, {}), 0u);
  reject(Model->call(kernel_api::KeInitializeMutex.str(), {Mutex, 0}));
  reject(Model->call(kernel_api::ExFreePool.str(), {Mutex}));
  reject(Model->call(kernel_api::ExFreePool.str(), {Blocks}),
         kernel_wait::BufferInUse);
  auto Access =
      Model->validateGuestAccess(Blocks + WaitBlockBytes - 1, 2, true);
  ASSERT_TRUE(bool(Access));
  EXPECT_EQ(llvm::toString(std::move(Access)), kernel_wait::OpaqueBlocks);
  EXPECT_FALSE(take(Model->pollWait(*Pending)));
  signal(Gate);
  EXPECT_EQ(take(Model->pollWait(*Pending)), windows::StatusSuccess);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateMutex, {Mutex}), 0u);
  EXPECT_EQ(call(kernel_api::KeAreApcsDisabled, {}), 1u);
  call(kernel_api::KeReleaseMutex, {Mutex, false});
  call(kernel_api::ExFreePool, {Blocks});
  call(kernel_api::KeInitializeMutex, {Mutex, 0});
}

TEST_F(KernelMultipleWait, WaitAnyChoosesLowestIndexAndConsumesOnlyThatObject) {
  const auto First = event(true);
  const auto Second = event(true);
  const auto Notification = event(true, true);
  put(Scratch + Stride * 3, 0);
  const auto Zero = Scratch + Stride * 3;
  EXPECT_EQ(take(wait({First, Second, Notification}, false, Zero)), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {First}), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Second}), 1u);
  EXPECT_EQ(take(wait({First, Second, Notification}, false, Zero)), 1u);
  EXPECT_EQ(take(wait({First, Second, Notification}, false, Zero)), 2u);
  EXPECT_EQ(take(wait({First, Second, Notification}, false, Zero)), 2u);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Notification}), 1u);
  EXPECT_FALSE(Model->takeWait());
}

TEST_F(KernelMultipleWait, DeferredWaitCapturesObjectsAndOriginalIndices) {
  const auto First = event();
  const auto Second = event();
  EXPECT_EQ(take(wait({First, Second}, false)), 0u);
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  const auto Replacement = event(true);
  put(Scratch, Replacement);
  signal(Second);
  EXPECT_EQ(take(Model->pollWait(*Pending)), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Replacement}), 1u);
  signal(First);
  reject(Model->pollWait(*Pending), kernel_wait::RegistrationLost);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {First}), 1u);
  call(kernel_api::KeInitializeEvent, {First, 1, false});
  call(kernel_api::KeInitializeEvent, {Second, 1, false});
}

TEST_F(KernelMultipleWait,
       CompletedWaitCannotConsumeAnotherRegistrationsSignal) {
  for (bool Timeout : {false, true}) {
    SCOPED_TRACE(Timeout);
    Model->enterExecution(profile::StackBase);
    const auto Event = event();
    const auto Interval = Scratch + Stride * 3;
    put(Interval, std::bit_cast<uint64_t>(-int64_t(TimeoutTicks)));
    EXPECT_EQ(take(wait({Event}, true, Timeout ? Interval : 0)), 0u);
    auto First = Model->takeWait();
    ASSERT_TRUE(First);
    Model->enterExecution(Scratch + Stride * 4);
    EXPECT_EQ(call(kernel_api::KeWaitForSingleObject,
                   {Event, windows::ExecutiveWaitReason, windows::KernelMode,
                    false, 0}),
              0u);
    auto Second = Model->takeWait();
    ASSERT_TRUE(Second);
    if (Timeout)
      ok(Model->advanceExecutionTo100ns(Model->now100ns() + TimeoutTicks));
    else
      signal(Event);
    EXPECT_EQ(take(Model->pollWait(*First)),
              Timeout ? windows::StatusTimeout : windows::StatusSuccess);
    signal(Event);
    reject(Model->pollWait(*First), kernel_wait::RegistrationLost);
    EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 1u);
    EXPECT_EQ(take(Model->pollWait(*Second)), windows::StatusSuccess);
    call(kernel_api::ExFreePool, {Event});
  }
}

TEST_F(KernelMultipleWait,
       CapturedWaitStateRejectsAlterationBeforeAcquisition) {
  const auto First = event();
  const auto Second = event();
  const auto Replacement = event(true);
  const auto Blocks = pool(2 * WaitBlockBytes);
  const auto Interval = Scratch + Stride * 3;
  put(Interval, std::bit_cast<uint64_t>(-int64_t(TimeoutTicks)));
  EXPECT_EQ(take(wait({First, Second}, false, Interval, Blocks)), 0u);
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  signal(Second);
  auto Changed = *Pending;
  Changed.Objects = {Replacement};
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.Thread ^= Stride;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.Execution ^= Stride;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.IRQL = scheduler::DispatchLevel;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.Deadline.reset();
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.All = true;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.WaitBlockArray = 0;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.Registration = 0;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  Changed = *Pending;
  Changed.Type = KernelModel::Wait::Kind::FrameworkIdle;
  reject(Model->pollWait(Changed), kernel_wait::RegistrationLost);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Replacement}), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Second}), 1u);
  EXPECT_EQ(take(Model->pollWait(*Pending)), 1u);
  call(kernel_api::ExFreePool, {Blocks});
}

TEST_F(KernelMultipleWait, CompletedDelayCannotReuseItsRegistration) {
  const auto Interval = Scratch + Stride * 3;
  put(Interval, std::bit_cast<uint64_t>(-int64_t(TimeoutTicks)));
  EXPECT_EQ(call(kernel_api::KeDelayExecutionThread,
                 {windows::KernelMode, false, Interval}),
            0u);
  auto First = Model->takeWait();
  ASSERT_TRUE(First);
  ok(Model->advanceExecutionTo100ns(TimeoutTicks));
  EXPECT_EQ(take(Model->pollWait(*First)), windows::StatusSuccess);
  EXPECT_EQ(call(kernel_api::KeDelayExecutionThread,
                 {windows::KernelMode, false, Interval}),
            0u);
  auto Second = Model->takeWait();
  ASSERT_TRUE(Second);
  reject(Model->pollWait(*First), kernel_wait::RegistrationLost);
  EXPECT_FALSE(take(Model->pollWait(*Second)));
  ok(Model->advanceExecutionTo100ns(2 * TimeoutTicks));
  EXPECT_EQ(take(Model->pollWait(*Second)), windows::StatusSuccess);
}

TEST_F(KernelMultipleWait, TimeoutReleasesEveryObjectAndCallerBufferReference) {
  const auto First = event(true);
  const auto Second = event();
  const auto Third = event(true);
  const auto Fourth = event(true);
  const auto Blocks = pool(4 * WaitBlockBytes);
  put(Scratch + Stride * 3, std::bit_cast<uint64_t>(-int64_t(TimeoutTicks)));
  EXPECT_EQ(take(wait({First, Second, Third, Fourth}, true,
                      Scratch + Stride * 3, Blocks)),
            0u);
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  ok(Model->advanceExecutionTo100ns(TimeoutTicks));
  EXPECT_EQ(take(Model->pollWait(*Pending)), windows::StatusTimeout);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {First}), 1u);
  for (auto Object : {First, Second, Third, Fourth})
    call(kernel_api::ExFreePool, {Object});
  call(kernel_api::ExFreePool, {Blocks});
}

TEST_F(KernelMultipleWait, InvalidLaterObjectCannotConsumeEarlierSignal) {
  const auto First = event(true);
  reject(wait({First, 0}, true), kernel_wait::InvalidObject);
  reject(wait({First, 0}, false), kernel_wait::InvalidObject);
  reject(wait({First, First}, true), kernel_wait::DuplicateObject);
  reject(wait({First, First}, false), kernel_wait::DuplicateObject);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {First}), 1u);
  EXPECT_FALSE(Model->takeWait());
  call(kernel_api::KeInitializeEvent, {First, 1, true});
}

TEST_F(KernelMultipleWait, DispatcherScalarParametersIgnoreUpperRegisterBits) {
  for (const uint64_t High : {ArgumentHighBits, ~uint64_t(UINT32_MAX)}) {
    SCOPED_TRACE(High);
    const auto Semaphore = pool(dispatcher::SemaphoreSize);
    auto Initialized = Model->call(kernel_api::KeInitializeSemaphore.str(),
                                   {Semaphore, High | 1, High | 2});
    ASSERT_TRUE(bool(Initialized)) << llvm::toString(Initialized.takeError());
    EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 1u);
    EXPECT_EQ(take(wait({Semaphore}, true)), windows::StatusSuccess);
    EXPECT_EQ(
        call(kernel_api::KeReleaseSemaphore, {Semaphore, High, High | 2, High}),
        0u);
    EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 2u);
    for (const uint64_t Adjustment : {High, High | UINT32_MAX}) {
      reject(Model->call(kernel_api::KeReleaseSemaphore.str(),
                         {Semaphore, High, Adjustment, High}));
      EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 2u);
    }
    reject(Model->call(kernel_api::KeReleaseSemaphore.str(),
                       {Semaphore, High, High | 1, High}));
    EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 2u);
    for (const uint64_t Count : {High | UINT32_MAX, High | 3}) {
      reject(Model->call(kernel_api::KeInitializeSemaphore.str(),
                         {Semaphore, Count, High | 2}));
      EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 2u);
    }
    for (const uint64_t Limit : {High, High | UINT32_MAX}) {
      reject(Model->call(kernel_api::KeInitializeSemaphore.str(),
                         {Semaphore, High | 1, Limit}));
      EXPECT_EQ(call(kernel_api::KeReadStateSemaphore, {Semaphore}), 2u);
    }

    const auto Mutex = pool(dispatcher::MutexSize);
    Initialized =
        Model->call(kernel_api::KeInitializeMutex.str(), {Mutex, High});
    ASSERT_TRUE(bool(Initialized)) << llvm::toString(Initialized.takeError());
    EXPECT_EQ(call(kernel_api::KeReadStateMutex, {Mutex}), 1u);
    EXPECT_EQ(take(wait({Mutex}, true)), windows::StatusSuccess);
    EXPECT_EQ(call(kernel_api::KeReleaseMutex, {Mutex, High}), 0u);
    reject(Model->call(kernel_api::KeInitializeMutex.str(), {Mutex, High | 1}));
    EXPECT_EQ(call(kernel_api::KeReadStateMutex, {Mutex}), 1u);
  }
}

TEST_F(KernelMultipleWait,
       MaximumCountUsesCallerBlocksAndPreservesBoundaryBytes) {
  std::vector<uint64_t> Objects;
  for (size_t I = 0; I < ObjectLimit; ++I)
    Objects.push_back(event(I == ObjectLimit - 1));
  const auto Blocks = pool(ObjectLimit * WaitBlockBytes + 2 * sizeof(uint64_t));
  put(Blocks, GuardByte);
  put(Blocks + ObjectLimit * WaitBlockBytes + sizeof(uint64_t), GuardByte);
  EXPECT_EQ(take(wait(Objects, false, 0, Blocks + sizeof(uint64_t))),
            ObjectLimit - 1);
  EXPECT_EQ(get(Blocks), GuardByte);
  EXPECT_EQ(get(Blocks + ObjectLimit * WaitBlockBytes + sizeof(uint64_t)),
            GuardByte);
  EXPECT_FALSE(Model->takeWait());
  reject(wait(Objects, true), kernel_wait::BlocksRequired);
  reject(Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                     {ObjectLimit + 1, Scratch, 0, 0, 0, 0, 0, Blocks}),
         kernel_wait::InvalidCount);
  reject(wait({}, true), kernel_wait::InvalidCount);
}

TEST_F(KernelMultipleWait,
       InvalidCallerStorageAndTypeHaveNoAcquisitionEffects) {
  const auto First = event(true);
  array({First});
  reject(Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                     {1, Scratch, 2, 0, 0, 0, 0, 0}),
         kernel_wait::InvalidType);
  reject(Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                     {1, Scratch, 0, 0, 1, 0, 0, 0}),
         kernel_wait::WaitMode);
  reject(Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                     {1, Scratch, 0, 0, 0, 1, 0, 0}),
         kernel_wait::WaitMode);
  reject(Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                     {1, Scratch, 0, 1, 0, 0, 0, 0}),
         kernel_wait::WaitReason);
  reject(Model->call(
             kernel_api::KeWaitForMultipleObjects.str(),
             {1, profile::UserProbeLimit - sizeof(uint64_t), 0, 0, 0, 0, 0, 0}),
         kernel_wait::ArrayStorage);
  reject(wait({First}, true, 0, profile::UserProbeLimit - WaitBlockBytes),
         kernel_wait::BlocksStorage);
  reject(wait({First}, true, 0, First));
  reject(wait({First}, true, 0, Scratch), kernel_wait::BufferOverlap);
  reject(wait({First}, true, 0, Scratch + 1), kernel_wait::BlocksStorage);
  const auto Paged = pool(WaitBlockBytes, 1);
  reject(wait({First}, true, 0, Paged));
  ok(Memory->protect(Scratch, profile::PageSize, Read));
  reject(Model->call(kernel_api::KeWaitForMultipleObjects.str(),
                     {1, Scratch, 0, 0, 0, 0, 0, Scratch + Stride}),
         kernel_wait::BlocksStorage);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {First}), 1u);
}

TEST_F(KernelMultipleWait, DispatchLevelAllowsOnlyZeroTimeoutPolling) {
  const auto First = event(true);
  const auto Second = event();
  put(Scratch + Stride * 3, 0);
  call(kernel_api::KfRaiseIrql, {scheduler::DispatchLevel});
  reject(wait({First, Second}, true), kernel_wait::BlockingIRQL);
  EXPECT_EQ(take(wait({First, Second}, true, Scratch + Stride * 3)),
            windows::StatusTimeout);
  EXPECT_EQ(take(wait({First, Second}, false, Scratch + Stride * 3)), 0u);
  EXPECT_FALSE(Model->takeWait());
  call(kernel_api::KeLowerIrql, {scheduler::PassiveLevel});
}

TEST_F(KernelMultipleWait, ThreadReferenceSurvivesHandleAndPointerRetirement) {
  call(kernel_api::PsCreateSystemThread,
       {Scratch + Stride, windows::ThreadAllAccess, 0, 0, 0,
        ImageBase + EntryOffset, 0});
  const auto Handle = get(Scratch + Stride);
  call(kernel_api::ObReferenceObjectByHandle,
       {Handle, 0, 0, windows::KernelMode, Scratch + Stride * 2, 0});
  const auto Thread = get(Scratch + Stride * 2);
  const auto Event = event(true);
  EXPECT_EQ(take(wait({Event, Thread}, true)), 0u);
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 1u);
  call(kernel_api::ZwClose, {Handle});
  call(kernel_api::ObfDereferenceObject, {Thread});
  EXPECT_FALSE(take(Model->pollWait(*Pending)));
  auto Invocation = take(Model->nextScheduled(false));
  ASSERT_TRUE(Invocation);
  Model->enterExecution(Scratch + Stride * 4, Invocation->ID);
  call(kernel_api::PsTerminateSystemThread, {windows::StatusSuccess});
  EXPECT_EQ(Model->takeThreadTermination(), windows::StatusSuccess);
  ok(Model->finishScheduled(Invocation->ID));
  EXPECT_EQ(take(Model->pollWait(*Pending)), windows::StatusSuccess);
  auto Access = Model->validateGuestAccess(Thread, 1, false);
  ASSERT_TRUE(bool(Access));
  llvm::consumeError(std::move(Access));
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 0u);
}

TEST_F(KernelMultipleWait, LateMutexIRQLFailureCannotPartiallyAcquireWaitAll) {
  const auto Event = event(true);
  const auto Mutex = pool(dispatcher::MutexSize);
  call(kernel_api::KeInitializeMutex, {Mutex, 0});
  EXPECT_EQ(take(wait({Mutex}, true)), windows::StatusSuccess);
  put(Scratch + Stride * 3, 0);
  call(kernel_api::KfRaiseIrql, {scheduler::DispatchLevel});
  reject(wait({Event, Mutex}, true, Scratch + Stride * 3),
         kernel_wait::RecursiveIRQL);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateMutex, {Mutex}), 0u);
  call(kernel_api::KeLowerIrql, {scheduler::PassiveLevel});
  EXPECT_EQ(call(kernel_api::KeReleaseMutex, {Mutex, false}), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateMutex, {Mutex}), 1u);
}
TEST_F(KernelMultipleWait,
       WaitAllRetainsBothTimerSignalsUntilEveryConditionIsReady) {
  const auto First = pool(dispatcher::TimerSize);
  const auto Second = pool(dispatcher::TimerSize);
  const auto Event = event();
  for (auto Timer : {First, Second})
    call(kernel_api::KeInitializeTimerEx, {Timer, 1});
  call(kernel_api::KeSetTimerEx,
       {First, std::bit_cast<uint64_t>(-int64_t(TimerTicks)), 0, 0});
  call(kernel_api::KeSetTimerEx,
       {Second, std::bit_cast<uint64_t>(-int64_t(TimeoutTicks)), 0, 0});
  EXPECT_EQ(take(wait({First, Second, Event}, true)), 0u);
  auto Pending = Model->takeWait();
  ASSERT_TRUE(Pending);
  ok(Model->advanceExecutionTo100ns(TimerTicks));
  EXPECT_FALSE(take(Model->pollWait(*Pending)));
  EXPECT_EQ(call(kernel_api::KeReadStateTimer, {First}), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateTimer, {Second}), 0u);
  ok(Model->advanceExecutionTo100ns(TimeoutTicks));
  EXPECT_FALSE(take(Model->pollWait(*Pending)));
  EXPECT_EQ(call(kernel_api::KeReadStateTimer, {First}), 1u);
  EXPECT_EQ(call(kernel_api::KeReadStateTimer, {Second}), 1u);
  signal(Event);
  EXPECT_EQ(take(Model->pollWait(*Pending)), windows::StatusSuccess);
  EXPECT_EQ(call(kernel_api::KeReadStateTimer, {First}), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateTimer, {Second}), 0u);
}

TEST_F(KernelMultipleWait,
       ExitedThreadWinsItsIndexWithoutConsumingAnotherSignal) {
  call(kernel_api::PsCreateSystemThread,
       {Scratch + Stride, windows::ThreadAllAccess, 0, 0, 0,
        ImageBase + EntryOffset, 0});
  const auto Handle = get(Scratch + Stride);
  call(kernel_api::ObReferenceObjectByHandle,
       {Handle, 0, 0, windows::KernelMode, Scratch + Stride * 2, 0});
  const auto Thread = get(Scratch + Stride * 2);
  auto Invocation = take(Model->nextScheduled(false));
  ASSERT_TRUE(Invocation);
  Model->enterExecution(Scratch + Stride * 4, Invocation->ID);
  reject(wait({Thread}, false), kernel_wait::SelfWait);
  call(kernel_api::PsTerminateSystemThread, {windows::StatusSuccess});
  EXPECT_EQ(Model->takeThreadTermination(), windows::StatusSuccess);
  ok(Model->finishScheduled(Invocation->ID));
  Model->enterExecution(profile::StackBase);
  const auto Event = event(true);
  EXPECT_EQ(take(wait({Thread, Event}, false)), 0u);
  EXPECT_EQ(call(kernel_api::KeReadStateEvent, {Event}), 1u);
  EXPECT_EQ(take(wait({Event, Thread}, false)), 0u);
  EXPECT_EQ(take(wait({Event, Thread}, false)), 1u);
  call(kernel_api::ZwClose, {Handle});
  call(kernel_api::ObfDereferenceObject, {Thread});
}
} // namespace
} // namespace neverd::emulation
