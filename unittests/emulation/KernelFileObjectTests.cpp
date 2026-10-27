//===- KernelFileObjectTests.cpp - File body ownership -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise independent scenario identities, caller references and CLOSE IRPs.

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
class KernelFileObjects : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t Device = 0;

  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> V) {
    if (!V) {
      ADD_FAILURE() << llvm::toString(V.takeError());
      return {};
    }
    return std::move(*V);
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    return take(Model->call(Name, Args));
  }
  uint64_t read(uint64_t Address, unsigned Size = 8) {
    return take(Memory->readInteger(Address, Size));
  }
  void write(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    ok(Model->validateGuestAccess(Address, Size, true));
    ok(Memory->writeInteger(Address, Value, Size));
  }
  void rejects(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, {}));
    Model->enterExecution(profile::StackBase);
    EXPECT_EQ(call("IoCreateDevice",
                   {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch}),
              0u);
    Device = read(Scratch);
    for (uint32_t Major : {0, 2, 3, 14, 18})
      write(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
  }
  KernelModel::Invocation begin(DriverRequestKind Kind, uint32_t ID = 1) {
    DriverRequest Input;
    Input.Kind = Kind;
    Input.File = ID;
    return take(Model->beginRequest(Input));
  }
  void complete(uint64_t IRP, uint32_t Status = 0) {
    write(IRP + IRPStatusOffset, Status, 4);
    write(IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {IRP, 0});
  }
  void finish(uint64_t IRP, uint32_t Status = 0) {
    complete(IRP, Status);
    ok(Model->recordDispatchReturn(IRP, Status));
    ok(Model->finalizeRequest(IRP));
  }
  uint64_t open(uint32_t ID = 1) {
    const auto Call = begin(DriverRequestKind::Create, ID);
    const auto File = read(Call.IRP + IRPOriginalFileOffset);
    finish(Call.IRP);
    return File;
  }
  void close(uint32_t ID = 1) {
    const auto Cleanup = begin(DriverRequestKind::Cleanup, ID);
    EXPECT_EQ(read(Cleanup.IRP + IRPRequestorModeOffset, 1), 0u);
    finish(Cleanup.IRP);
    const auto Close = begin(DriverRequestKind::Close, ID);
    if (Close.IRP)
      finish(Close.IRP);
  }
  KernelGuestCall release(uint64_t File) {
    call("ObfDereferenceObject", {File});
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    ok(Model->beginGuestCall(Call->Token));
    return *Call;
  }
};

TEST_F(KernelFileObjects, ReferencesDuringCreateRetainOneTypedBody) {
  const auto Create = begin(DriverRequestKind::Create);
  const uint64_t File = read(Create.IRP + IRPOriginalFileOffset);
  const uint64_t Type = call("ObGetObjectType", {File});
  const uint64_t WrongType = call("ObGetObjectType", {Device});
  EXPECT_NE(Type, WrongType);
  EXPECT_EQ(call("ObReferenceObjectByPointer", {File, 0, WrongType, 0}),
            0xc0000024u);
  EXPECT_EQ(call("ObReferenceObjectByPointer", {File, 0xffffffff, Type, 0}),
            0u);
  EXPECT_EQ(call("ObReferenceObjectByPointer", {File, 0, Type, 1}), 0u);
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 1u);
  finish(Create.IRP);
  write(File + FileContextOffset, 0x12345678);
  close();
  EXPECT_EQ(read(File + FileContextOffset), 0x12345678u);
  EXPECT_TRUE(read(File + FileFlagsOffset, 4) & FileCleanupComplete);
  EXPECT_FALSE(Result.Requests.back().Completed);
  EXPECT_EQ(Result.Requests.back().IRP, 0u);
  call("ObfDereferenceObject", {File});
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 1u);
  const auto Call = release(File);
  ASSERT_NE(Call.PC, 0u);
  EXPECT_EQ(read(Call.Arguments[1] + IRPOriginalFileOffset), File);
  EXPECT_EQ(read(Call.Arguments[1] + IRPRequestorModeOffset, 1), 0u);
  auto Revive = Model->call("ObfReferenceObject", {File});
  ASSERT_FALSE(bool(Revive));
  rejects(Revive.takeError(), "final CLOSE");
  complete(Call.Arguments[1]);
  ok(Model->validateGuestAccess(File, 2, false));
  EXPECT_EQ(take(Model->finishGuestCall(Call.Token, 0)),
            std::optional<uint64_t>{0});
  rejects(Model->validateGuestAccess(File, 2, false), "freed");
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
  EXPECT_TRUE(Result.Requests.back().Completed);
}

TEST_F(KernelFileObjects, ReusedScenarioIDDoesNotReplaceRetainedBody) {
  const uint64_t Old = open();
  call("ObfReferenceObject", {Old});
  close();
  const size_t CloseIndex = Result.Requests.size() - 1;
  const uint64_t New = open();
  ASSERT_NE(New, Old);
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 2u);
  const auto Call = release(Old);
  ASSERT_NE(Call.PC, 0u);
  EXPECT_EQ(read(Call.Arguments[1] + IRPOriginalFileOffset), Old);
  complete(Call.Arguments[1]);
  take(Model->finishGuestCall(Call.Token, 0));
  EXPECT_TRUE(Result.Requests[CloseIndex].Completed);
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 1u);
  const auto Read = begin(DriverRequestKind::Read);
  EXPECT_EQ(read(Read.IRP + IRPOriginalFileOffset), New);
  finish(Read.IRP);
  close();
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
}

TEST_F(KernelFileObjects,
       PendingCloseWaitsForCompletionAndRetainsDeletedDevice) {
  const uint64_t File = open();
  call("ObfReferenceObject", {File});
  close();
  call("IoDeleteDevice", {Device});
  const auto Call = release(File);
  ASSERT_NE(Call.PC, 0u);
  const uint64_t IRP = Call.Arguments[1];
  const uint64_t Stack = read(IRP + IRPStackPointerOffset);
  write(Stack + StackControlOffset, StackPendingReturned, 1);
  EXPECT_EQ(take(Model->finishGuestCall(Call.Token, StatusPending)),
            std::optional<uint64_t>{0});
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_EQ(Wait->Type, KernelModel::Wait::Kind::FileClose);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  ok(Model->validateGuestAccess(File, 2, false));
  ok(Model->validateGuestAccess(Device, 2, false));
  complete(IRP);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  rejects(Model->validateGuestAccess(File, 2, false), "freed");
  rejects(Model->validateGuestAccess(Device, 2, false), "freed");
}

TEST_F(KernelFileObjects, FailedCreateKeepsCallerBodyWithoutCloseOrDeviceHold) {
  const auto Create = begin(DriverRequestKind::Create);
  const uint64_t File = read(Create.IRP + IRPOriginalFileOffset);
  call("ObfReferenceObject", {File});
  call("IoDeleteDevice", {Device});
  finish(Create.IRP, 0xc0000001);
  EXPECT_EQ(read(File + FileDeviceOffset), 0u);
  ok(Model->validateGuestAccess(File, 2, false));
  rejects(Model->validateGuestAccess(Device, 2, false), "freed");
  call("ObfDereferenceObject", {File});
  EXPECT_FALSE(Model->takeGuestCall());
  rejects(Model->validateGuestAccess(File, 2, false), "freed");
  EXPECT_EQ(Result.Requests.size(), 1u);
}

TEST_F(KernelFileObjects, ElevatedFinalReleaseFailsBeforeConsumingReference) {
  const uint64_t File = open();
  call("ObfReferenceObject", {File});
  close();
  const auto Previous = call("KfRaiseIrql", {2});
  auto Released = Model->call("ObfDereferenceObject", {File});
  ASSERT_FALSE(bool(Released));
  rejects(Released.takeError(), "PASSIVE_LEVEL");
  call("KeLowerIrql", {Previous});
  const auto Call = release(File);
  ASSERT_NE(Call.PC, 0u);
  complete(Call.Arguments[1]);
  take(Model->finishGuestCall(Call.Token, 0));
}

TEST_F(KernelFileObjects, UnknownReleaseContextDoesNotInventSystemProcess) {
  const uint64_t File = open();
  call("ObfReferenceObject", {File});
  close();
  Model->enterExecution(profile::CallbackStackBase);
  auto Released = Model->call("ObfDereferenceObject", {File});
  ASSERT_FALSE(bool(Released));
  rejects(Released.takeError(), "known process context");
  Model->enterExecution(profile::StackBase);
  const auto Call = release(File);
  ASSERT_NE(Call.PC, 0u);
  complete(Call.Arguments[1]);
  take(Model->finishGuestCall(Call.Token, 0));
}

TEST_F(KernelFileObjects, ReparseDoesNotBecomeASuccessfulOpen) {
  const auto Create = begin(DriverRequestKind::Create);
  write(Create.IRP + IRPStatusOffset, 0x104, 4);
  write(Create.IRP + IRPInformationOffset, 0);
  auto Completed = Model->call("IofCompleteRequest", {Create.IRP, 0});
  ASSERT_FALSE(bool(Completed));
  rejects(Completed.takeError(), "reparse");
  EXPECT_FALSE(Result.Requests.back().Completed);
}

TEST_F(KernelFileObjects, HandlesSeparateProcessUnlockFromGlobalCleanup) {
  const uint64_t File = open();
  const uint64_t Type = call("ObGetObjectType", {File});
  EXPECT_EQ(call("ObOpenObjectByPointer",
                 {File, 0x200, 0, 0x80000000, Type, 0, Scratch + 8}),
            0u);
  const uint64_t First = read(Scratch + 8);
  EXPECT_EQ(call("ObOpenObjectByPointer",
                 {File, 0x200, 0, 0x40000000, 0, 0, Scratch + 8}),
            0u);
  const uint64_t Last = read(Scratch + 8);
  EXPECT_EQ(call("ObReferenceObjectByHandle",
                 {First, 0, Type, 0, Scratch + 16, Scratch + 24}),
            0u);
  EXPECT_EQ(read(Scratch + 16), File);
  EXPECT_EQ(read(Scratch + 28, 4), 0x120089u);
  call("ObfDereferenceObject", {File});
  EXPECT_EQ(
      call("ObReferenceObjectByHandle", {First, 0, Type, 1, Scratch + 16, 0}),
      0xc0000008u);
  EXPECT_EQ(call("ZwClose", {First}), 0u);
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(Result.Requests.size(), 1u);
  const auto Unlock = begin(DriverRequestKind::Cleanup);
  ASSERT_EQ(Unlock.PC, 0u); // Default unsupported-major dispatch actually ran.
  ASSERT_EQ(Result.Requests.size(), 3u);
  EXPECT_EQ(Result.Requests[1].IRP, 0u);
  EXPECT_FALSE(Result.Requests[1].Completed);
  EXPECT_EQ(Result.Requests[2].Kind, DriverRequestKind::UnlockAll);
  EXPECT_EQ(Result.Requests[2].Origin, DriverRequestOrigin::FileHandleClose);
  EXPECT_TRUE(Result.Requests[2].Completed);
  EXPECT_EQ(begin(DriverRequestKind::Close).IRP, 0u);
  EXPECT_EQ(call("ZwClose", {Last}), 0u);
  auto Cleanup = Model->takeGuestCall();
  ASSERT_TRUE(Cleanup);
  EXPECT_EQ(Cleanup->ProcessID, std::optional<uint32_t>{4});
  ok(Model->beginGuestCall(Cleanup->Token));
  const uint64_t IRP = Cleanup->Arguments[1];
  EXPECT_EQ(read(read(IRP + IRPStackPointerOffset), 1), 18u);
  complete(IRP);
  EXPECT_FALSE(take(Model->finishGuestCall(Cleanup->Token, 0)));
  auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  ok(Model->beginGuestCall(Close->Token));
  complete(Close->Arguments[1]);
  EXPECT_EQ(take(Model->finishGuestCall(Close->Token, 0)),
            std::optional<uint64_t>{0});
  EXPECT_TRUE(Result.Requests[1].Completed);
  EXPECT_EQ(Result.Requests[1].RequestorProcessID, 4u);
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
  rejects(Model->validateGuestAccess(File, 2, false), "freed");
  EXPECT_EQ(call("ZwClose", {Last}), 0xc0000008u);
}

TEST_F(KernelFileObjects, PendingHandleCleanupResumesItsOriginalCaller) {
  const uint64_t File = open();
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {File, 0x200, 0, 0, 0, 0, Scratch + 8}),
      0u);
  const uint64_t Handle = read(Scratch + 8);
  begin(DriverRequestKind::Cleanup);
  begin(DriverRequestKind::Close);
  ok(Model->setUserRequestContext(true, 0x2000));
  const auto Caller = Model->captureExecutionContext();
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  auto Cleanup = Model->takeGuestCall();
  ASSERT_TRUE(Cleanup);
  ok(Model->beginGuestCall(Cleanup->Token));
  const uint64_t Child = profile::CallbackStackBase;
  ok(Model->inheritExecutionContext(Child, profile::StackBase,
                                    Cleanup->ProcessID));
  Model->enterExecution(Child, profile::StackBase, Cleanup->Token);
  ok(Model->activateInheritedProcess());
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), 0x2000u);
  EXPECT_EQ(call("PsGetProcessId", {call("IoGetCurrentProcess", {})}), 4u);
  EXPECT_EQ(call("ExGetPreviousMode", {}), 1u);
  const uint64_t IRP = Cleanup->Arguments[1];
  write(read(IRP + IRPStackPointerOffset) + StackControlOffset,
        StackPendingReturned, 1);
  EXPECT_EQ(take(Model->finishGuestCall(Cleanup->Token, 0x103)),
            std::optional<uint64_t>{0});
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  complete(IRP);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  EXPECT_FALSE(Model->takeGuestCall());
  ok(Model->restoreExecutionContext(Caller));
  EXPECT_FALSE(take(Model->resumeFileHandleClose(Wait->Object)));
  auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  EXPECT_EQ(Close->ProcessID, std::optional<uint32_t>{0x2000});
  EXPECT_EQ(Result.Requests[1].RequestorProcessID, 4u);
  EXPECT_EQ(Result.Requests[3].RequestorProcessID, 0x2000u);
  ok(Model->beginGuestCall(Close->Token));
  complete(Close->Arguments[1]);
  take(Model->finishGuestCall(Close->Token, 0));
  EXPECT_EQ(call("PsGetCurrentProcessId", {}), 0x2000u);
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
}

TEST_F(KernelFileObjects, CleanedPointerCanAcquireAndCloseAnotherHandle) {
  const uint64_t File = open();
  call("ObfReferenceObject", {File});
  close();
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {File, 0x200, 0, 0, 0, 0, Scratch + 8}),
      0u);
  call("ObfDereferenceObject", {File});
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(call("ZwClose", {read(Scratch + 8)}), 0u);
  auto Cleanup = Model->takeGuestCall();
  ASSERT_TRUE(Cleanup);
  EXPECT_EQ(Result.Requests.back().Origin,
            DriverRequestOrigin::FileHandleClose);
  ok(Model->beginGuestCall(Cleanup->Token));
  complete(Cleanup->Arguments[1]);
  EXPECT_FALSE(take(Model->finishGuestCall(Cleanup->Token, 0)));
  auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  ok(Model->beginGuestCall(Close->Token));
  complete(Close->Arguments[1]);
  take(Model->finishGuestCall(Close->Token, 0));
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
}

TEST_F(KernelFileObjects, HandleCloseFailurePreservesItsReference) {
  const uint64_t File = open();
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {File, 0x200, 0, 0, 0, 0, Scratch + 8}),
      0u);
  const uint64_t Handle = read(Scratch + 8);
  Model->enterExecution(profile::CallbackStackBase);
  auto Bad = Model->call("ZwClose", {Handle});
  ASSERT_FALSE(bool(Bad));
  rejects(Bad.takeError(), "known PASSIVE_LEVEL caller");
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(
      call("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 16, 0}),
      0u);
  EXPECT_EQ(read(Scratch + 16), File);
  call("ObfDereferenceObject", {File});
  EXPECT_EQ(call("ZwClose", {Handle}), 0u); // Unregistered unlock dispatch.
  EXPECT_FALSE(Model->takeGuestCall());
  close();
}

TEST_F(KernelFileObjects, PointerOpenRequiresInitialHandleCreation) {
  const auto Create = begin(DriverRequestKind::Create);
  const uint64_t File = read(Create.IRP + IRPOriginalFileOffset);
  auto Bad = Model->call("ObOpenObjectByPointer",
                         {File, 0x200, 0, 0, 0, 0, Scratch + 8});
  ASSERT_FALSE(bool(Bad));
  rejects(Bad.takeError(), "initial successful handle");
  finish(Create.IRP);
  close();
}

TEST_F(KernelFileObjects, InvalidCloseRoutePreservesHandleForRetry) {
  const uint64_t File = open();
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {File, 0x200, 0, 0, 0, 0, Scratch + 8}),
      0u);
  const uint64_t Handle = read(Scratch + 8);
  begin(DriverRequestKind::Cleanup);
  begin(DriverRequestKind::Close);
  write(Device + DeviceStackCountOffset, 0, 1);
  auto Bad = Model->call("ZwClose", {Handle});
  ASSERT_FALSE(bool(Bad));
  rejects(Bad.takeError(), "StackSize must be a positive CCHAR");
  EXPECT_EQ(
      call("ObReferenceObjectByHandle", {Handle, 0, 0, 0, Scratch + 16, 0}),
      0u);
  EXPECT_EQ(read(Scratch + 16), File);
  call("ObfDereferenceObject", {File});
  write(Device + DeviceStackCountOffset, 1, 1);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  auto Cleanup = Model->takeGuestCall();
  ASSERT_TRUE(Cleanup);
  ok(Model->beginGuestCall(Cleanup->Token));
  complete(Cleanup->Arguments[1]);
  EXPECT_FALSE(take(Model->finishGuestCall(Cleanup->Token, 0)));
  auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  ok(Model->beginGuestCall(Close->Token));
  complete(Close->Arguments[1]);
  take(Model->finishGuestCall(Close->Token, 0));
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
}

TEST_F(KernelFileObjects, ScenarioLifecycleCallbacksRejectPointerReopening) {
  for (bool Clone : {false, true}) {
    SCOPED_TRACE(Clone);
    const uint64_t File = open();
    if (Clone)
      EXPECT_EQ(
          call("ObOpenObjectByPointer", {File, 0x200, 0, 0, 0, 0, Scratch + 8}),
          0u);
    const uint64_t Handle = Clone ? read(Scratch + 8) : 0;
    write(Model->driverObject() + DriverDispatchOffset + 17 * 8, Entry);
    const auto Callback = begin(DriverRequestKind::Cleanup);
    ASSERT_NE(Callback.IRP, 0u);
    auto Bad = Model->call("ObOpenObjectByPointer",
                           {File, 0x200, 0, 0, 0, 0, Scratch + 16});
    ASSERT_FALSE(bool(Bad));
    rejects(Bad.takeError(), "handle-close callback");
    complete(Callback.IRP);
    Bad = Model->call("ObOpenObjectByPointer",
                      {File, 0x200, 0, 0, 0, 0, Scratch + 16});
    ASSERT_FALSE(bool(Bad));
    rejects(Bad.takeError(), "handle-close callback");
    ok(Model->recordDispatchReturn(Callback.IRP, 0));
    ok(Model->finalizeRequest(Callback.IRP));
    const auto Close = begin(DriverRequestKind::Close);
    if (!Clone) {
      finish(Close.IRP);
      continue;
    }
    EXPECT_EQ(call("ZwClose", {Handle}), 0u);
    auto Cleanup = Model->takeGuestCall();
    ASSERT_TRUE(Cleanup);
    ok(Model->beginGuestCall(Cleanup->Token));
    complete(Cleanup->Arguments[1]);
    EXPECT_FALSE(take(Model->finishGuestCall(Cleanup->Token, 0)));
    auto Final = Model->takeGuestCall();
    ASSERT_TRUE(Final);
    ok(Model->beginGuestCall(Final->Token));
    complete(Final->Arguments[1]);
    take(Model->finishGuestCall(Final->Token, 0));
  }
}

TEST_F(KernelFileObjects, ExitedRequestorStillUnlocksRetainedFile) {
  const uint64_t File = open();
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {File, 0x200, 0, 0, 0, 0, Scratch + 8}),
      0u);
  const uint64_t Handle = read(Scratch + 8);
  DriverRequest Input;
  Input.Kind = DriverRequestKind::DeviceControl;
  Input.File = 1;
  Input.ControlCode = 0x222003;
  Input.Input = {0x41};
  Input.RequestorExitAfterDispatch = true;
  auto Transfer = take(Model->beginRequest(Input));
  complete(Transfer.IRP);
  ok(Model->recordDispatchReturn(Transfer.IRP, 0));
  ok(Model->exitRequestorProcess(Transfer.IRP));
  ok(Model->finalizeRequest(Transfer.IRP));
  begin(DriverRequestKind::Cleanup);
  EXPECT_EQ(Result.Requests.back().Kind, DriverRequestKind::UnlockAll);
  EXPECT_TRUE(Result.Requests.back().Completed);
  begin(DriverRequestKind::Close);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  auto Cleanup = Model->takeGuestCall();
  ASSERT_TRUE(Cleanup);
  ok(Model->beginGuestCall(Cleanup->Token));
  complete(Cleanup->Arguments[1]);
  EXPECT_FALSE(take(Model->finishGuestCall(Cleanup->Token, 0)));
  auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  ok(Model->beginGuestCall(Close->Token));
  complete(Close->Arguments[1]);
  take(Model->finishGuestCall(Close->Token, 0));
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
}
} // namespace
} // namespace neverd::emulation
