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
} // namespace
} // namespace neverd::emulation
