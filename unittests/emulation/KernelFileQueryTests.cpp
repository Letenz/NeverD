//===- KernelFileQueryTests.cpp - Internal query ownership ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify borrowed query packets, completion publication and caller lifetime.

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
bool ntError(uint32_t Status) {
  return (Status >> profile::NTStatusSeverityShift) ==
         profile::NTStatusErrorSeverity;
}
class KernelFileQueries : public testing::Test {
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
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A) {
    return take(Model->call(Name, A));
  }
  void write(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model->validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t read(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
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
    constexpr char16_t Name[] = u"\\Device\\FileQueryTest";
    write(Scratch + 0x20, sizeof(Name) - 2, 2);
    write(Scratch + 0x22, sizeof(Name), 2);
    write(Scratch + 0x28, Scratch + 0x40);
    ok(Memory->write(
        Scratch + 0x40,
        llvm::ArrayRef(reinterpret_cast<const uint8_t *>(Name), sizeof(Name))));
    call("IoCreateDevice",
         {Model->driverObject(), 0, Scratch + 0x20, 0x22, 0, 0, Scratch});
    Device = read(Scratch);
    for (uint32_t Major : {0, 2, 5, 14, 18})
      write(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
  }
  KernelModel::Invocation begin(DriverRequestKind Kind, bool Async = false) {
    DriverRequest Input;
    Input.Kind = Kind;
    Input.File = 1;
    Input.Device = "\\Device\\FileQueryTest";
    if (Kind == DriverRequestKind::Create)
      Input.AsynchronousFile = Async;
    return take(Model->beginRequest(Input));
  }
  void complete(uint64_t IRP, uint32_t Status = 0, uint64_t Information = 0) {
    write(IRP + IRPStatusOffset, Status, 4);
    write(IRP + IRPInformationOffset, Information);
    call("IofCompleteRequest", {IRP, 0});
  }
  void finish(uint64_t IRP) {
    complete(IRP);
    ok(Model->recordDispatchReturn(IRP, 0));
    ok(Model->finalizeRequest(IRP));
  }
  uint64_t open(bool Async = false) {
    auto Create = begin(DriverRequestKind::Create, Async);
    const auto File = read(Create.IRP + IRPOriginalFileOffset);
    finish(Create.IRP);
    return File;
  }
  void close() {
    finish(begin(DriverRequestKind::Cleanup).IRP);
    auto Close = begin(DriverRequestKind::Close);
    if (Close.IRP)
      finish(Close.IRP);
  }
  struct Query {
    uint64_t ID, IRP;
    KernelGuestCall Call;
  };
  Query query(uint64_t File, uint32_t Class = 5, uint32_t Length = 64,
              uint64_t Buffer = Scratch + 0x100,
              uint64_t Returned = Scratch + 0x200) {
    call("IoQueryFileInformation", {File, Class, Length, Buffer, Returned});
    const auto ID = Model->takeFileQueryContinuation();
    const auto Call = Model->takeGuestCall();
    EXPECT_TRUE(ID);
    EXPECT_TRUE(Call);
    if (!ID || !Call)
      return {};
    ok(Model->beginGuestCall(Call->Token));
    return {*ID, Call->Arguments[1], *Call};
  }
};

TEST_F(KernelFileQueries, PacketAndPublicationMatchTargetQueryAndCompletion) {
  struct Outcome {
    uint32_t Dispatch, Complete;
  };
  for (uint32_t Class : {4, 5, 6, 9, 14, 75})
    for (bool Async : {false, true})
      for (auto Outcome :
           {Outcome{0, 0}, Outcome{0x103, 0}, Outcome{0x103, 0x80000005},
            Outcome{0, 0xc0000001}, Outcome{0x103, 0xc0000001}})
        for (bool Alias : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << Class << ' ' << Async << ' ' << Outcome.Dispatch
                       << ' ' << Outcome.Complete << ' ' << Alias);
          const auto File = open(Async);
          const uint64_t Buffer = Scratch + 0x100;
          const uint64_t Returned = Alias ? Buffer + 4 : Scratch + 0x200;
          write(Returned, 0xaabbccdddeadbeef);
          ok(Memory->writeInteger(File + FileFinalStatusOffset, 0x77777777, 4));
          const auto Q = query(File, Class, 64, Buffer, Returned);
          ASSERT_NE(Q.IRP, 0u);
          EXPECT_EQ(call("KeAreApcsDisabled", {}), Async ? 0u : 1u);
          EXPECT_EQ(call("KeAreAllApcsDisabled", {}), 0u);
          const auto Stack = read(Q.IRP + IRPStackPointerOffset);
          const auto IOSB = read(Q.IRP + IRPUserIosbOffset);
          const auto Event = read(Q.IRP + IRPUserEventOffset);
          EXPECT_EQ(read(Q.IRP + IRPFlagsOffset, 4), Async ? 0x14u : 0x10u);
          EXPECT_EQ(read(Q.IRP + IRPAllocationFlagsOffset, 1), Async ? 0u : 2u);
          EXPECT_EQ(read(Q.IRP + IRPRequestorModeOffset, 1), 0u);
          EXPECT_EQ(read(Q.IRP + IRPSystemBufferOffset), Buffer);
          EXPECT_EQ(read(Q.IRP + IRPOriginalFileOffset), File);
          EXPECT_EQ(read(Q.IRP + IRPThreadOffset),
                    call("PsGetCurrentThread", {}));
          EXPECT_EQ(bool(Event), Async);
          EXPECT_EQ(read(Stack, 1), 5u);
          EXPECT_EQ(read(Stack + StackFlagsOffset, 1), Class == 75 ? 1u : 0u);
          EXPECT_EQ(read(Stack + StackQueryClassOffset, 4),
                    Class == 75 ? 71u : Class);
          EXPECT_EQ(read(Stack + StackQueryLengthOffset, 4), 64u);
          EXPECT_EQ(read(Stack + StackFileOffset), File);
          if (Outcome.Dispatch == StatusPending)
            write(Stack + StackControlOffset, 1, 1);
          const std::array<uint8_t, 7> Bytes{0x11, 0x22, 0x33, 0x44,
                                             0x55, 0x66, 0x77};
          ok(Memory->write(Buffer, Bytes));
          complete(Q.IRP, Outcome.Complete, 7);
          const bool Published =
              !ntError(Outcome.Complete) || Outcome.Dispatch == StatusPending;
          EXPECT_EQ(read(IOSB, 4), Published ? Outcome.Complete : 0u);
          EXPECT_EQ(read(IOSB + 8), Published ? 7u : 0u);
          EXPECT_EQ(read(File + FileFinalStatusOffset, 4),
                    Published && !Async ? Outcome.Complete : 0x77777777u);
          if (Async)
            EXPECT_EQ(call("KeReadStateEvent", {Event}), Published ? 1u : 0u);
          ok(Model->validateGuestAccess(Buffer, 64, true));
          rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
          take(Model->finishGuestCall(Q.Call.Token, Outcome.Dispatch));
          EXPECT_FALSE(Model->takeWait());
          EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
                    std::optional<uint64_t>{Outcome.Dispatch == StatusPending
                                                ? Outcome.Complete
                                                : Outcome.Dispatch});
          EXPECT_EQ(read(Returned, 4), Published ? 7u : 0u);
          EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
          EXPECT_EQ(read(Returned + 4, 4), 0xaabbccddu);
          ok(Model->validateGuestAccess(Buffer, 64, true));
          rejects(Model->validateGuestAccess(IOSB, 1, false), "freed");
          EXPECT_EQ(Result.Requests.back().FileInformationClass, Class);
          EXPECT_EQ(Result.Requests.back().Output.size(),
                    ntError(Outcome.Complete) ? 0u : 7u);
          close();
          EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
        }
}

TEST_F(KernelFileQueries,
       PendingQueryAndFinalCloseResumeOnlyTheirOriginalCaller) {
  const auto File = open();
  call("ObfReferenceObject", {File});
  close();
  ok(Model->setUserRequestContext(true, 0x2000));
  write(Scratch + 0x200, 0xdeadbeef, 4);
  const auto Q = query(File);
  const auto IOSB = read(Q.IRP + IRPUserIosbOffset);
  const auto FinalStatus = read(File + FileFinalStatusOffset, 4);
  write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
  // The internal hold permits final pointer release while the query is live.
  const auto OldIRQL = call("KfRaiseIrql", {2});
  call("ObfDereferenceObject", {File});
  call("KeLowerIrql", {OldIRQL});
  EXPECT_FALSE(Model->takeGuestCall());
  take(Model->finishGuestCall(Q.Call.Token, StatusPending));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(profile::CallbackStackBase);
  complete(Q.IRP, 0xc0000001, 0x100000007ull);
  EXPECT_EQ(read(IOSB + 8), 0u);
  EXPECT_EQ(read(File + FileFinalStatusOffset, 4), FinalStatus);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  auto Wrong = Model->resumeFileInformationQuery(Q.ID);
  ASSERT_FALSE(bool(Wrong));
  rejects(Wrong.takeError(), "original caller");
  Model->enterExecution(profile::StackBase);
  EXPECT_FALSE(take(Model->resumeFileInformationQuery(Q.ID)));
  const auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  EXPECT_EQ(Close->ProcessID, 0x2000u);
  EXPECT_EQ(read(Scratch + 0x200, 4), 0xdeadbeefu);
  ok(Model->beginGuestCall(Close->Token));
  const uint64_t IRP = Close->Arguments[1];
  write(read(IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
  take(Model->finishGuestCall(Close->Token, StatusPending));
  const auto CloseWait = Model->takeWait();
  ASSERT_TRUE(CloseWait);
  EXPECT_FALSE(take(Model->pollWait(*CloseWait)));
  complete(IRP);
  EXPECT_EQ(take(Model->pollWait(*CloseWait)), std::optional<uint32_t>{0});
  EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
            std::optional<uint64_t>{0xc0000001u});
  EXPECT_EQ(read(Scratch + 0x200, 4), 7u);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
}

TEST_F(KernelFileQueries,
       AsynchronousLastQueryReferenceRequiresDeferredDeletion) {
  const auto File = open(true);
  call("ObfReferenceObject", {File});
  close();
  const auto Q = query(File);
  auto Released = Model->call("ObfDereferenceObject", {File});
  ASSERT_FALSE(bool(Released));
  rejects(Released.takeError(), "deferred file deletion");
  complete(Q.IRP);
  take(Model->finishGuestCall(Q.Call.Token, 0));
  EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
            std::optional<uint64_t>{0});
  call("ObfDereferenceObject", {File});
  const auto Close = Model->takeGuestCall();
  ASSERT_TRUE(Close);
  ok(Model->beginGuestCall(Close->Token));
  complete(Close->Arguments[1]);
  take(Model->finishGuestCall(Close->Token, 0));
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
}

TEST_F(KernelFileQueries,
       NotificationsPublishWhenSpecialApcsBecomeDeliverable) {
  for (bool Async : {false, true})
    for (bool Guarded : {false, true}) {
      const auto File = open(Async);
      call("KeEnterCriticalRegion", {});
      const auto Q = query(File);
      const auto IOSB = read(Q.IRP + IRPUserIosbOffset);
      const auto Event =
          Async ? read(Q.IRP + IRPUserEventOffset) : File + FileEventOffset;
      if (Guarded) {
        call("KeEnterGuardedRegion", {});
        call("KeEnterGuardedRegion", {});
      } else {
        EXPECT_EQ(call("KfRaiseIrql", {2}), 0u);
      }
      complete(Q.IRP, 0, 7);
      EXPECT_EQ(read(IOSB + 8), 0u);
      EXPECT_EQ(call("KeReadStateEvent", {Event}), 0u);
      if (Guarded) {
        call("KeLeaveGuardedRegion", {});
        EXPECT_EQ(read(IOSB + 8), 0u);
        call("KeLeaveGuardedRegion", {});
      } else {
        call("KeLowerIrql", {0});
      }
      EXPECT_EQ(read(IOSB + 8), 7u);
      EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
      take(Model->finishGuestCall(Q.Call.Token, 0));
      EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
                std::optional<uint64_t>{0});
      EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
      call("KeLeaveCriticalRegion", {});
      EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
      close();
    }
}

TEST_F(KernelFileQueries,
       AllocationFailureResetsEventAndPreservesCallerOutput) {
  const auto File = open();
  const auto Q = query(File);
  complete(Q.IRP);
  take(Model->finishGuestCall(Q.Call.Token, 0));
  take(Model->resumeFileInformationQuery(Q.ID));
  EXPECT_EQ(call("KeReadStateEvent", {File + FileEventOffset}), 1u);
  for (uint64_t Size : {4096u, 16u})
    while (call("ExAllocatePoolWithTag", {0, Size, 0x51554552})) {
    }
  const auto Count = Result.Requests.size();
  write(Scratch + 0x200, 0xaabbccdddeadbeef);
  call("KeEnterCriticalRegion", {});
  EXPECT_EQ(call("IoQueryFileInformation", {File, 5, 0, 0, Scratch + 0x200}),
            StatusInsufficientResources);
  EXPECT_EQ(read(Scratch + 0x200), 0xaabbccdddeadbeefull);
  EXPECT_EQ(call("KeReadStateEvent", {File + FileEventOffset}), 0u);
  EXPECT_EQ(Result.Requests.size(), Count);
  EXPECT_FALSE(Model->takeFileQueryContinuation());
  EXPECT_FALSE(Model->takeGuestCall());
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
  call("KeLeaveCriticalRegion", {});
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
}

TEST_F(KernelFileQueries, EntryRequiresPassiveLevelAndSpecialApcs) {
  const auto File = open();
  call("KeEnterGuardedRegion", {});
  auto Guarded =
      Model->call("IoQueryFileInformation", {File, 5, 0, 0, Scratch});
  ASSERT_FALSE(bool(Guarded));
  rejects(Guarded.takeError(), "special kernel APCs");
  call("KeLeaveGuardedRegion", {});
  call("KfRaiseIrql", {1});
  auto Raised = Model->call("IoQueryFileInformation", {File, 5, 0, 0, Scratch});
  ASSERT_FALSE(bool(Raised));
  rejects(Raised.takeError(), "PASSIVE_LEVEL");
  call("KeLowerIrql", {0});
  EXPECT_FALSE(Model->takeFileQueryContinuation());
  EXPECT_FALSE(Model->takeGuestCall());
  close();
}

TEST_F(KernelFileQueries, CompletionOnlyReadsTheBorrowedReportBuffer) {
  const auto File = open();
  const auto Q = query(File, 5, 64, Scratch + 0x1000);
  write(Scratch + 0x1000, 0x123456789abcdef0);
  ok(Memory->protect(Scratch + 0x1000, 0x1000, Read));
  complete(Q.IRP, 0, 8);
  take(Model->finishGuestCall(Q.Call.Token, 0));
  EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
            std::optional<uint64_t>{0});
  EXPECT_EQ(read(Scratch + 0x200, 4), 8u);
  EXPECT_EQ(
      Result.Requests.back().Output,
      (std::vector<uint8_t>{0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12}));
  close();
}

TEST_F(KernelFileQueries,
       CompletionRejectsOversizedSuccessButReportsErrorLength) {
  const auto File = open();
  const auto Q = query(File, 5, 1);
  write(Q.IRP + IRPStatusOffset, 0, 4);
  write(Q.IRP + IRPInformationOffset, 2);
  auto Oversized = Model->call("IofCompleteRequest", {Q.IRP, 0});
  ASSERT_FALSE(bool(Oversized));
  rejects(Oversized.takeError(), "borrowed output buffer");
  ok(Model->validateGuestAccess(Q.IRP + IRPStatusOffset, 4, true));
  write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
  take(Model->finishGuestCall(Q.Call.Token, StatusPending));
  ASSERT_TRUE(Model->takeWait());
  complete(Q.IRP, 0xc0000004, 0x1234567800000030ull);
  EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
            std::optional<uint64_t>{0xc0000004});
  EXPECT_EQ(read(Scratch + 0x200, 4), 0x30u);
  close();
}

TEST_F(KernelFileQueries, RevalidatesReturnedLengthAfterPendingCompletion) {
  const auto File = open();
  const auto Q = query(File, 5, 0, 0, Scratch + 0xfffc);
  write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
  take(Model->finishGuestCall(Q.Call.Token, StatusPending));
  ASSERT_TRUE(Model->takeWait());
  complete(Q.IRP);
  ok(Memory->protect(Scratch + 0xf000, 0x1000, Read));
  auto Resume = Model->resumeFileInformationQuery(Q.ID);
  ASSERT_FALSE(bool(Resume));
  rejects(Resume.takeError(), "writable");
  ok(Memory->protect(Scratch + 0xf000, 0x1000, Read | Write));
  EXPECT_EQ(take(Model->resumeFileInformationQuery(Q.ID)),
            std::optional<uint64_t>{0});
  close();
}

TEST_F(KernelFileQueries, RejectsInvalidOutputsIdentityModeAndSameFileOverlap) {
  const auto File = open();
  for (auto Args :
       {std::array<uint64_t, 5>{Device, 5, 8, Scratch, Scratch + 8},
        std::array<uint64_t, 5>{File, 5, 8, Scratch + 0xfffc, Scratch},
        std::array<uint64_t, 5>{File, 5, 8, Scratch, Scratch + 0xfffe}}) {
    auto Rejected = Model->call("IoQueryFileInformation", Args);
    ASSERT_FALSE(bool(Rejected));
    llvm::consumeError(Rejected.takeError());
    EXPECT_FALSE(Model->takeFileQueryContinuation());
    EXPECT_FALSE(Model->takeGuestCall());
  }
  const auto Flags = read(File + FileFlagsOffset, 4);
  ok(Memory->writeInteger(File + FileFlagsOffset, Flags ^ FileSynchronousIO,
                          4));
  auto Mode = Model->call("IoQueryFileInformation", {File, 5, 0, 0, Scratch});
  ASSERT_FALSE(bool(Mode));
  rejects(Mode.takeError(), "synchronous mode");
  ok(Memory->writeInteger(File + FileFlagsOffset, Flags, 4));
  auto IO = begin(DriverRequestKind::DeviceControl);
  auto Overlap =
      Model->call("IoQueryFileInformation", {File, 5, 0, 0, Scratch});
  ASSERT_FALSE(bool(Overlap));
  rejects(Overlap.takeError(), "same-file");
  finish(IO.IRP);
  close();
}

TEST_F(KernelFileQueries,
       RelatedDeviceUsesTopologyAndRejectsUnownedMountHints) {
  call("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
  const auto Upper = read(Scratch);
  EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Upper, Device}), Device);
  const auto File = open();
  EXPECT_EQ(call("IoGetRelatedDeviceObject", {File}), Upper);
  for (auto Address : {File + FileVPBOffset, File + FileExtensionOffset,
                       Device + DeviceVPBOffset}) {
    ok(Memory->writeInteger(Address, Scratch, 8));
    auto Rejected = Model->call("IoGetRelatedDeviceObject", {File});
    ASSERT_FALSE(bool(Rejected));
    rejects(Rejected.takeError(), "unsupported");
    ok(Memory->writeInteger(Address, 0, 8));
  }
  ok(Memory->writeInteger(File + FileDeviceOffset, Upper, 8));
  auto Changed = Model->call("IoGetRelatedDeviceObject", {File});
  ASSERT_FALSE(bool(Changed));
  rejects(Changed.takeError(), "identity");
  ok(Memory->writeInteger(File + FileDeviceOffset, Device, 8));
  close();
  call("IoDetachDevice", {Device});
  call("IoDeleteDevice", {Upper});
}
} // namespace
} // namespace neverd::emulation
