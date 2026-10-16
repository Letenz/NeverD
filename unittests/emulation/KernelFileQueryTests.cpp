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

#include "llvm/Support/Endian.h"

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
    const auto ID = Model->takeFileRequestContinuation();
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
          EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
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

TEST_F(KernelFileQueries, NativeQueryOwnsBufferAndDefersImmediatePublication) {
  struct Outcome {
    uint32_t Dispatch, Complete;
  };
  for (uint32_t Class : {4, 5, 6, 9, 75})
    for (bool Async : {false, true})
      for (auto Outcome :
           {Outcome{0, 0}, Outcome{0, 0xc0000001},
            Outcome{0x80000005, 0x80000005}, Outcome{StatusPending, 0},
            Outcome{StatusPending, 0xc0000001}})
        for (bool Alias : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << Class << ' ' << Async << ' ' << Outcome.Dispatch
                       << ' ' << Outcome.Complete << ' ' << Alias);
          const auto File = open(Async);
          EXPECT_EQ(
              call("ObOpenObjectByPointer", {File, ObjectKernelHandle, 0, 0, 0,
                                             KernelMode, Scratch + 0x300}),
              0u);
          const auto Handle = read(Scratch + 0x300);
          const uint64_t Buffer = Scratch + 0x400;
          const uint64_t StatusOut = Alias ? Buffer + 4 : Scratch + 0x500;
          std::vector<uint8_t> Expected(64, 0xa5);
          ok(Memory->write(Buffer, Expected));
          ok(Memory->write(StatusOut, std::vector<uint8_t>(16, 0xa5)));
          call("ZwQueryInformationFile",
               {Handle, StatusOut, Buffer, 64, Class});
          auto ID = Model->takeFileRequestContinuation();
          auto Call = Model->takeGuestCall();
          ASSERT_TRUE(ID);
          ASSERT_TRUE(Call);
          ok(Model->beginGuestCall(Call->Token));
          const auto IRP = Call->Arguments[1];
          const auto System = read(IRP + IRPSystemBufferOffset);
          const auto IOSB = read(IRP + IRPUserIosbOffset);
          EXPECT_NE(System, Buffer);
          EXPECT_EQ(read(IRP + IRPUserBufferOffset), Buffer);
          EXPECT_EQ(read(IRP + IRPFlagsOffset, 4), Async ? 0x874u : 0x870u);
          EXPECT_EQ(read(IRP + IRPAllocationFlagsOffset, 1), Async ? 0u : 2u);
          EXPECT_EQ(IOSB == StatusOut, !Async);
          EXPECT_EQ(read(System), 0u);
          const bool Pending = Outcome.Dispatch == StatusPending;
          if (Pending)
            write(read(IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
          const std::array<uint8_t, 7> Bytes{0x11, 0x22, 0x33, 0x44,
                                             0x55, 0x66, 0x77};
          ok(Memory->write(System, Bytes));
          complete(IRP, Outcome.Complete, 7);
          if (!Pending) {
            EXPECT_EQ(read(StatusOut), 0xa5a5a5a5a5a5a5a5ull);
            EXPECT_EQ(read(Buffer), 0xa5a5a5a5a5a5a5a5ull);
            ok(Model->validateGuestAccess(IRP + IRPFlagsOffset, 4, false));
            rejects(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, true),
                    "opaque");
            ok(Model->validateGuestAccess(System, 7, true));
          } else {
            rejects(Model->validateGuestAccess(IRP, 4, false), "freed");
            rejects(Model->validateGuestAccess(System, 1, false), "freed");
          }
          take(Model->finishGuestCall(Call->Token, Outcome.Dispatch));
          ASSERT_FALSE(HasFailure());
          EXPECT_EQ(take(Model->resumeFileRequest(*ID)),
                    std::optional<uint64_t>{Pending ? Outcome.Complete
                                                    : Outcome.Dispatch});
          ASSERT_FALSE(HasFailure());
          if (!ntError(Outcome.Complete))
            std::copy(Bytes.begin(), Bytes.end(), Expected.begin());
          std::array<uint8_t, 16> ExpectedStatus;
          ExpectedStatus.fill(0xa5);
          if (!ntError(Outcome.Complete) || Pending) {
            if (Async && Pending)
              ExpectedStatus.fill(0);
            llvm::support::endian::write32le(ExpectedStatus.data(),
                                             Outcome.Complete);
            llvm::support::endian::write64le(ExpectedStatus.data() + 8, 7);
          }
          if (Alias)
            std::copy(ExpectedStatus.begin(), ExpectedStatus.end(),
                      Expected.begin() + 4);
          std::vector<uint8_t> Observed(64);
          std::array<uint8_t, 16> ObservedStatus;
          ok(Memory->read(Buffer, Observed));
          ok(Memory->read(StatusOut, ObservedStatus));
          EXPECT_EQ(Observed, Expected);
          EXPECT_EQ(ObservedStatus, ExpectedStatus);
          EXPECT_EQ(Result.Requests.back().Origin,
                    DriverRequestOrigin::FileHandleQuery);
          EXPECT_EQ(Result.Requests.back().Output,
                    ntError(Outcome.Complete)
                        ? std::vector<uint8_t>{}
                        : std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
          rejects(Model->validateGuestAccess(IRP, 1, false), "freed");
          rejects(Model->validateGuestAccess(System, 1, false), "freed");
          ok(Model->validateGuestAccess(Buffer, 64, true));
          EXPECT_EQ(call("ZwClose", {Handle}), 0u);
          close();
        }
}

TEST_F(KernelFileQueries, NativeLocalClassesUseHandleScopeAndExactOutputs) {
  const auto File = open();
  EXPECT_EQ(call("ObOpenObjectByPointer",
                 {File, 0, 0, 0x80, 0, KernelMode, Scratch + 0x300}),
            0u);
  const auto Handle = read(Scratch + 0x300);
  const auto Before = Result.Requests.size();
  for (auto [Class, Size, Value] :
       {std::tuple{8u, 4u, 0x80ull}, std::tuple{14u, 8u, 0ull},
        std::tuple{51u, 1u, 0ull}}) {
    write(Scratch + 0x400, 0xa5a5a5a5a5a5a5a5ull);
    EXPECT_EQ(call("ZwQueryInformationFile",
                   {Handle, Scratch + 0x500, Scratch + 0x400, Size - 1, Class}),
              StatusInfoLengthMismatch);
    EXPECT_EQ(read(Scratch + 0x400), 0xa5a5a5a5a5a5a5a5ull);
    EXPECT_EQ(call("ZwQueryInformationFile",
                   {Handle, Scratch + 0x500, Scratch + 0x400, Size, Class}),
              0u);
    EXPECT_EQ(read(Scratch + 0x400, Size), Value);
    EXPECT_EQ(read(Scratch + 0x508), Size);
    EXPECT_FALSE(Model->takeFileRequestContinuation());
  }
  EXPECT_EQ(Result.Requests.size(), Before);
  ok(Model->setUserRequestContext(true, 0x1234));
  EXPECT_EQ(call("ZwQueryInformationFile", {Handle, 0, 0, 4, 8}),
            StatusInvalidHandle);
  ok(Model->setUserRequestContext(false, 0));
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  close();
}

TEST_F(KernelFileQueries,
       NativeWorkerCompletionWaitsForCallerAndKeepsFullInformation) {
  const auto File = open(true);
  call("ObOpenObjectByPointer",
       {File, ObjectKernelHandle, 0, 0, 0, KernelMode, Scratch + 0x300});
  const auto Handle = read(Scratch + 0x300);
  ok(Model->setUserRequestContext(true, 0x2000));
  write(Scratch + 0x400, 0x123456789abcdef0ull);
  write(Scratch + 0x500, 0xa5a5a5a5a5a5a5a5ull);
  write(Scratch + 0x508, 0xa5a5a5a5a5a5a5a5ull);
  call("ZwQueryInformationFile",
       {Handle, Scratch + 0x500, Scratch + 0x400, 64, 5});
  auto ID = Model->takeFileRequestContinuation();
  auto Call = Model->takeGuestCall();
  ASSERT_TRUE(ID);
  ASSERT_TRUE(Call);
  ok(Model->beginGuestCall(Call->Token));
  const auto IRP = Call->Arguments[1];
  const auto System = read(IRP + IRPSystemBufferOffset);
  write(read(IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
  take(Model->finishGuestCall(Call->Token, StatusPending));
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  Model->enterExecution(profile::CallbackStackBase);
  complete(IRP, 0xc0000001, 0x100000007ull);
  EXPECT_EQ(read(Scratch + 0x500), 0xa5a5a5a5a5a5a5a5ull);
  ok(Model->validateGuestAccess(IRP + IRPFlagsOffset, 4, false));
  ok(Model->validateGuestAccess(System, 1, false));
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(take(Model->resumeFileRequest(*ID)),
            std::optional<uint64_t>{0xc0000001});
  EXPECT_EQ(read(Scratch + 0x500), 0xc0000001u);
  EXPECT_EQ(read(Scratch + 0x508), 0x100000007ull);
  EXPECT_EQ(read(Scratch + 0x400), 0x123456789abcdef0ull);
  rejects(Model->validateGuestAccess(IRP, 1, false), "freed");
  rejects(Model->validateGuestAccess(System, 1, false), "freed");
  call("ZwClose", {Handle});
  close();
}

TEST_F(KernelFileQueries,
       NativeAdmissionRejectsUnsupportedRoutesBeforeCreatingPackets) {
  const auto File = open();
  call("ObOpenObjectByPointer",
       {File, ObjectKernelHandle, 0, 0, 0, KernelMode, Scratch + 0x300});
  const auto Handle = read(Scratch + 0x300);
  const auto Count = Result.Requests.size();
  const auto Check = [&](uint32_t Class, llvm::StringRef Reason) {
    SCOPED_TRACE(::testing::Message() << Class << ' ' << Reason.str());
    auto Result =
        Model->call("ZwQueryInformationFile",
                    {Handle, Scratch + 0x500, Scratch + 0x400, 64, Class});
    ASSERT_FALSE(bool(Result));
    rejects(Result.takeError(), Reason);
    EXPECT_FALSE(Model->takeFileRequestContinuation());
    EXPECT_FALSE(Model->takeGuestCall());
  };
  Check(18, "not implemented"); // FILE_ALL_INFORMATION has producer fields.
  ok(Memory->writeInteger(Model->driverObject() + DriverFastIOOffset,
                          Scratch + 0x800, 8));
  Check(4, "Fast I/O");
  ok(Memory->writeInteger(Model->driverObject() + DriverFastIOOffset, 0, 8));
  write(Model->driverObject() + DriverDispatchOffset + 5 * 8, 0);
  Check(5, "null file dispatch callback");
  for (uint32_t Class : {16u, 17u}) {
    call("ZwQueryInformationFile",
         {Handle, Scratch + 0x500, Scratch + 0x400, 4, Class});
    auto ID = Model->takeFileRequestContinuation();
    ASSERT_TRUE(ID);
    EXPECT_FALSE(Model->takeGuestCall());
    EXPECT_EQ(take(Model->resumeFileRequest(*ID)), std::optional<uint64_t>{0});
    EXPECT_EQ(read(Scratch + 0x508), 4u);
  }
  write(Model->driverObject() + DriverDispatchOffset + 5 * 8, Entry);
  call("KeEnterGuardedRegion", {});
  Check(8, "special kernel APCs");
  call("KeLeaveGuardedRegion", {});
  call("KfRaiseIrql", {1});
  Check(8, "PASSIVE_LEVEL");
  call("KeLowerIrql", {0});
  EXPECT_EQ(Result.Requests.size(), Count + 2);
  const auto IO = begin(DriverRequestKind::DeviceControl);
  Check(14, "same-file"); // The local position query takes the file lock too.
  Check(5, "same-file");
  finish(IO.IRP);
  call("ZwClose", {Handle});
  close();
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
  auto Wrong = Model->resumeFileRequest(Q.ID);
  ASSERT_FALSE(bool(Wrong));
  rejects(Wrong.takeError(), "original caller");
  Model->enterExecution(profile::StackBase);
  EXPECT_FALSE(take(Model->resumeFileRequest(Q.ID)));
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
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
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
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
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
      EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
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
  take(Model->resumeFileRequest(Q.ID));
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
  EXPECT_FALSE(Model->takeFileRequestContinuation());
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
  EXPECT_FALSE(Model->takeFileRequestContinuation());
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
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
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
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
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
  auto Resume = Model->resumeFileRequest(Q.ID);
  ASSERT_FALSE(bool(Resume));
  rejects(Resume.takeError(), "writable");
  ok(Memory->protect(Scratch + 0xf000, 0x1000, Read | Write));
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
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
    EXPECT_FALSE(Model->takeFileRequestContinuation());
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

TEST_F(KernelFileQueries, BaseDeviceBorrowsFileDeviceBelowAttachedFilters) {
  uint64_t Lower = Device;
  std::array<uint64_t, 2> Filters{};
  for (auto &Filter : Filters) {
    call("IoCreateDevice", {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch});
    Filter = read(Scratch);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Filter, Device}), Lower);
    Lower = Filter;
  }
  const auto File = open();
  const auto References = read(Device + DeviceReferenceCount, 4);
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {File}), Device);
    EXPECT_EQ(call("IoGetRelatedDeviceObject", {File}), Filters.back());
    EXPECT_EQ(read(Device + DeviceReferenceCount, 4), References);
    for (auto Filter : Filters)
      EXPECT_EQ(read(Filter + DeviceReferenceCount, 4), 0u);
  }
  // The base query does not route through, or dereference, this field.
  ok(Memory->writeInteger(File + FileExtensionOffset, UINT64_MAX, 8));
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {File}), Device);
  auto Related = Model->call("IoGetRelatedDeviceObject", {File});
  ASSERT_FALSE(bool(Related));
  rejects(Related.takeError(), "file-extension routing");
  ok(Memory->writeInteger(File + FileExtensionOffset, 0, 8));
  close();
  auto Closed = Model->call("IoGetBaseFileSystemDeviceObject", {File});
  ASSERT_FALSE(bool(Closed));
  rejects(Closed.takeError(), "live file");
  call("IoDetachDevice", {Filters.front()});
  call("IoDeleteDevice", {Filters.back()});
  call("IoDetachDevice", {Device});
  call("IoDeleteDevice", {Filters.front()});
  EXPECT_EQ(read(Device + DeviceReferenceCount, 4), 0u);
}

TEST_F(KernelFileQueries, BaseDevicePreservesFileAdmissionAndIrqlBounds) {
  auto Create = begin(DriverRequestKind::Create);
  const auto File = read(Create.IRP + IRPOriginalFileOffset);
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {File}), Device);
  finish(Create.IRP);
  for (auto Args : {std::vector<uint64_t>{}, std::vector<uint64_t>{File, 0},
                    std::vector<uint64_t>{0}, std::vector<uint64_t>{Scratch},
                    std::vector<uint64_t>{File + 1}}) {
    auto Bad = Model->call("IoGetBaseFileSystemDeviceObject", Args);
    ASSERT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (uint8_t Level : {uint8_t(1), uint8_t(2)}) {
    EXPECT_EQ(call("KfRaiseIrql", {Level}), 0u);
    EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {File}), Device);
    call("KeLowerIrql", {0});
  }
  call("KfRaiseIrql", {3});
  auto High = Model->call("IoGetBaseFileSystemDeviceObject", {File});
  ASSERT_FALSE(bool(High));
  rejects(High.takeError(), "IRQL");
  call("KeLowerIrql", {0});
  finish(begin(DriverRequestKind::Cleanup).IRP);
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {File}), Device);
  auto Close = begin(DriverRequestKind::Close);
  auto Closing = Model->call("IoGetBaseFileSystemDeviceObject", {File});
  ASSERT_FALSE(bool(Closing));
  rejects(Closing.takeError(), "live file");
  finish(Close.IRP);
}

TEST_F(KernelFileQueries, BaseDeviceRejectsUnownedRoutesAndChangedRecords) {
  const auto File = open();
  for (auto Address : {File + FileVPBOffset, Device + DeviceVPBOffset}) {
    ok(Memory->writeInteger(Address, Scratch, 8));
    auto Mounted = Model->call("IoGetBaseFileSystemDeviceObject", {File});
    ASSERT_FALSE(bool(Mounted));
    rejects(Mounted.takeError(), "mounted VPB routing");
    ok(Memory->writeInteger(Address, 0, 8));
  }
  for (auto Address : {File, File + 2}) {
    const auto Saved = read(Address, 2);
    ok(Memory->writeInteger(Address, 0, 2));
    auto Header = Model->call("IoGetBaseFileSystemDeviceObject", {File});
    ASSERT_FALSE(bool(Header));
    rejects(Header.takeError(), "header");
    ok(Memory->writeInteger(Address, Saved, 2));
  }
  ok(Memory->writeInteger(File + FileDeviceOffset, Scratch, 8));
  auto Identity = Model->call("IoGetBaseFileSystemDeviceObject", {File});
  ASSERT_FALSE(bool(Identity));
  rejects(Identity.takeError(), "identity");
  ok(Memory->writeInteger(File + FileDeviceOffset, Device, 8));
  ok(Memory->writeInteger(Device + DeviceAttachedOffset, Scratch, 8));
  auto Topology = Model->call("IoGetBaseFileSystemDeviceObject", {File});
  ASSERT_FALSE(bool(Topology));
  llvm::consumeError(Topology.takeError());
  ok(Memory->writeInteger(Device + DeviceAttachedOffset, 0, 8));
  EXPECT_EQ(call("IoGetBaseFileSystemDeviceObject", {File}), Device);
  close();
}

class KernelFileTransfers : public KernelFileQueries {
protected:
  void SetUp() override {
    KernelFileQueries::SetUp();
    write(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    for (unsigned Major : {3, 4, 9, 13})
      write(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
  }
  uint64_t handle(uint64_t File, uint32_t Access = 0) {
    EXPECT_EQ(
        call("ObOpenObjectByPointer", {File, ObjectKernelHandle, 0, Access, 0,
                                       KernelMode, Scratch + 0x300}),
        0u);
    return read(Scratch + 0x300);
  }
  Query transfer(bool Write, uint64_t Handle, uint64_t Length = 16,
                 uint64_t Buffer = Scratch + 0x400,
                 uint64_t IOSB = Scratch + 0x500,
                 uint64_t Offset = Scratch + 0x600,
                 uint64_t Key = Scratch + 0x610) {
    call(Write ? "ZwWriteFile" : "ZwReadFile",
         {Handle, 0, 0, 0, IOSB, Buffer, Length, Offset, Key});
    auto ID = Model->takeFileRequestContinuation();
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(ID);
    EXPECT_TRUE(Call);
    if (!ID || !Call)
      return {};
    ok(Model->beginGuestCall(Call->Token));
    return {*ID, Call->Arguments[1], *Call};
  }
  Query flush(uint64_t Handle, uint64_t IOSB = Scratch + 0x500) {
    call("ZwFlushBuffersFile", {Handle, IOSB});
    auto ID = Model->takeFileRequestContinuation();
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(ID);
    EXPECT_TRUE(Call);
    if (!ID || !Call)
      return {};
    ok(Model->beginGuestCall(Call->Token));
    return {*ID, Call->Arguments[1], *Call};
  }
  Query control(bool FS, uint64_t Handle, uint64_t InputLength = 16,
                uint64_t OutputLength = 16, uint64_t Code = 0x22e000,
                uint64_t Input = Scratch + 0x400,
                uint64_t Output = Scratch + 0x480,
                uint64_t IOSB = Scratch + 0x500) {
    call(FS ? "ZwFsControlFile" : "ZwDeviceIoControlFile",
         {Handle, 0, 0, 0, IOSB, Code, Input, InputLength, Output,
          OutputLength});
    auto ID = Model->takeFileRequestContinuation();
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(ID);
    EXPECT_TRUE(Call);
    if (!ID || !Call)
      return {};
    ok(Model->beginGuestCall(Call->Token));
    return {*ID, Call->Arguments[1], *Call};
  }
};

TEST_F(KernelFileTransfers, BufferedPacketsAndCallerPublicationMatchTarget) {
  for (bool Write : {false, true})
    for (unsigned Length : {0, 16})
      for (bool Pending : {false, true})
        for (uint32_t Status : {0u, 0x80000005u, 0xc0000001u})
          for (bool Alias : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << Write << ' ' << Length << ' ' << Pending << ' '
                         << Status << ' ' << Alias);
            const auto File = open(), Handle = handle(File);
            constexpr uint64_t Buffer = Scratch + 0x400, Position = 0x123456789;
            const uint64_t IOSB = Alias ? Buffer + 4 : Scratch + 0x500;
            ok(Memory->write(Buffer, std::vector<uint8_t>(64, 0xa5)));
            ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
            ok(Memory->writeInteger(File + FileCurrentByteOffset, Position, 8));
            write(Scratch + 0x600, 0x100000005);
            write(Scratch + 0x610, 0x89abcdef, 4);
            const auto Q =
                transfer(Write, Handle, 0x100000000ull + Length, Buffer, IOSB);
            ASSERT_NE(Q.IRP, 0u);
            const auto Stack = read(Q.IRP + IRPStackPointerOffset);
            const auto System = read(Q.IRP + IRPSystemBufferOffset);
            EXPECT_EQ(read(Q.IRP + IRPFlagsOffset, 4),
                      0x60000u | (Write ? (Length ? 0xa30u : 0xa10u)
                                        : (Length ? 0x970u : 0x950u)));
            EXPECT_EQ(read(Q.IRP + IRPAllocationFlagsOffset, 1), 2u);
            EXPECT_EQ(read(Q.IRP + IRPUserIosbOffset), IOSB);
            EXPECT_EQ(read(Q.IRP + IRPUserEventOffset), 0u);
            EXPECT_EQ(read(Q.IRP + IRPUserBufferOffset),
                      !Write && Length ? Buffer : 0u);
            EXPECT_EQ(read(Q.IRP + IRPOriginalFileOffset), File);
            EXPECT_EQ(read(Stack, 1), Write ? 4u : 3u);
            EXPECT_EQ(read(Stack + 8, 4), Length);
            EXPECT_EQ(read(Stack + 16, 4), 0x89abcdefu);
            EXPECT_EQ(read(Stack + 24), 0x100000005u);
            EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
            EXPECT_EQ(call("KeAreAllApcsDisabled", {}), 0u);
            const uint64_t Count = Length ? 7 : 0;
            std::vector<uint8_t> Bytes{1, 2, 3, 4, 5, 6, 7};
            if (Length) {
              EXPECT_NE(System, Buffer);
              if (Write)
                EXPECT_EQ(read(System), 0xa5a5a5a5a5a5a5a5ull);
              ok(Memory->write(System, Bytes));
            } else {
              EXPECT_EQ(System, 0u);
            }
            if (Pending)
              write(Stack + StackControlOffset, 1, 1);
            complete(Q.IRP, Status, Count);
            if (!Pending) {
              EXPECT_EQ(read(IOSB), 0xa5a5a5a5a5a5a5a5ull);
              EXPECT_EQ(read(Buffer), 0xa5a5a5a5a5a5a5a5ull);
              ok(Model->validateGuestAccess(Q.IRP + IRPFlagsOffset, 4, false));
            }
            take(Model->finishGuestCall(Q.Call.Token,
                                        Pending ? StatusPending : Status));
            EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                      std::optional<uint64_t>{Status});
            std::vector<uint8_t> Expected(64, 0xa5);
            if (!Write && Length && !ntError(Status))
              std::copy(Bytes.begin(), Bytes.end(), Expected.begin());
            std::array<uint8_t, 16> ExpectedStatus;
            ExpectedStatus.fill(0xa5);
            if (!ntError(Status) || Pending) {
              llvm::support::endian::write32le(ExpectedStatus.data(), Status);
              llvm::support::endian::write64le(ExpectedStatus.data() + 8,
                                               Count);
            }
            if (Alias)
              std::copy(ExpectedStatus.begin(), ExpectedStatus.end(),
                        Expected.begin() + 4);
            std::vector<uint8_t> Observed(64);
            std::array<uint8_t, 16> ObservedStatus;
            ok(Memory->read(Buffer, Observed));
            ok(Memory->read(IOSB, ObservedStatus));
            EXPECT_EQ(Observed, Expected);
            EXPECT_EQ(ObservedStatus, ExpectedStatus);
            EXPECT_EQ(read(File + FileCurrentByteOffset), Position);
            EXPECT_EQ(Result.Requests.back().Origin,
                      Write ? DriverRequestOrigin::FileHandleWrite
                            : DriverRequestOrigin::FileHandleRead);
            EXPECT_EQ(Result.Requests.back().ByteOffset, 0x100000005u);
            EXPECT_FALSE(Result.Requests.back().FileInformationClass);
            EXPECT_EQ(Result.Requests.back().Output,
                      !Write && Length && !ntError(Status)
                          ? Bytes
                          : std::vector<uint8_t>{});
            rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
            if (System)
              rejects(Model->validateGuestAccess(System, 1, false), "freed");
            ok(Model->validateGuestAccess(Buffer, 64, true));
            EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
            call("ZwClose", {Handle});
            close();
            ASSERT_FALSE(HasFailure());
          }
}

TEST_F(KernelFileTransfers, OffsetCaptureAndAppendOnlyHandlesMatchProducer) {
  for (bool Write : {false, true})
    for (uint32_t Access : {0, 2, 4, 6})
      for (std::optional<uint64_t> Offset :
           {std::optional<uint64_t>{}, std::optional<uint64_t>{0},
            std::optional<uint64_t>{0x100000005},
            std::optional<uint64_t>{UINT64_MAX},
            std::optional<uint64_t>{uint64_t(-2)},
            std::optional<uint64_t>{uint64_t(-3)}}) {
        const auto File = open(), Handle = handle(File, Access);
        constexpr uint64_t Position = 0x123456789;
        ok(Memory->writeInteger(File + FileCurrentByteOffset, Position, 8));
        const auto Address = Offset ? Scratch + 0x600 : 0;
        if (Offset)
          write(Address, *Offset);
        const uint64_t Expected = Write && Access == 4 ? UINT64_MAX
                                  : !Offset || *Offset == uint64_t(-2)
                                      ? Position
                                      : *Offset;
        if (Expected > INT64_MAX && !(Write && Expected == UINT64_MAX)) {
          EXPECT_EQ(call(Write ? "ZwWriteFile" : "ZwReadFile",
                         {Handle, 0, 0, 0, Scratch + 0x500, 0, 0, Address, 0}),
                    StatusInvalidParameter);
          EXPECT_FALSE(Model->takeGuestCall());
          EXPECT_FALSE(Model->takeFileRequestContinuation());
        } else {
          const auto Q =
              transfer(Write, Handle, 0, 0, Scratch + 0x500, Address, 0);
          ASSERT_NE(Q.IRP, 0u);
          EXPECT_EQ(read(read(Q.IRP + IRPStackPointerOffset) + 24), Expected);
          complete(Q.IRP);
          take(Model->finishGuestCall(Q.Call.Token, 0));
          EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                    std::optional<uint64_t>{0});
        }
        call("ZwClose", {Handle});
        close();
        ASSERT_FALSE(HasFailure());
      }
}

TEST_F(KernelFileTransfers, WriteCapturesReadOnlyInputAndPreservesItsBytes) {
  const auto File = open(), Handle = handle(File);
  constexpr uint64_t Source = Scratch + 0xe000, Bytes = 0x1122334455667788;
  write(Source, Bytes);
  ok(Memory->protect(Source, 0x1000, Read));
  const auto Flags = read(File + FileFlagsOffset, 4);
  ok(Memory->writeInteger(File + FileFlagsOffset, Flags | 0x18, 4));
  write(Scratch + 0x600, 5); // KernelMode does not add a user-mode probe.
  const auto Q =
      transfer(true, Handle, 8, Source, Scratch + 0x500, Scratch + 0x600, 0);
  ASSERT_NE(Q.IRP, 0u);
  const auto System = read(Q.IRP + IRPSystemBufferOffset);
  EXPECT_EQ(read(System), Bytes);
  EXPECT_EQ(read(Q.IRP + IRPUserBufferOffset), 0u);
  EXPECT_EQ(read(Q.IRP + IRPFlagsOffset, 4), 0x60a31u);
  EXPECT_EQ(read(read(Q.IRP + IRPStackPointerOffset) + StackFlagsOffset, 1),
            4u);
  write(System, 0);
  complete(Q.IRP, 0, 8);
  take(Model->finishGuestCall(Q.Call.Token, 0));
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
  EXPECT_EQ(read(Source), Bytes);
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  ok(Memory->protect(Source, 0x1000, Read | Write));
  ok(Memory->writeInteger(File + FileFlagsOffset, Flags, 4));
  call("ZwClose", {Handle});
  close();
}

TEST_F(KernelFileTransfers,
       InvalidOffsetReadFaultStopsBeforePacketPublication) {
  const auto File = open(), Handle = handle(File);
  const auto Before = Result.Requests.size();
  auto Fault =
      Model->call("ZwReadFile", {Handle, 0, 0, 0, Scratch + 0x500,
                                 Scratch + 0x400, 16, Scratch + 0xffff, 0});
  ASSERT_FALSE(bool(Fault));
  llvm::consumeError(Fault.takeError());
  EXPECT_TRUE(Memory->hasMemoryFault());
  EXPECT_EQ(Result.Requests.size(), Before);
  EXPECT_FALSE(Model->takeFileRequestContinuation());
  EXPECT_FALSE(Model->takeGuestCall());
  // A faulted CPU is terminal. Do not simulate more guest calls on it.
}

TEST_F(KernelFileTransfers, WorkerCompletionRetainsCallerOutputUntilDelivery) {
  const auto File = open(), Handle = handle(File);
  ok(Model->setUserRequestContext(true, 0x2000));
  write(Scratch + 0x400, 0xa5a5a5a5a5a5a5a5ull);
  write(Scratch + 0x500, 0xa5a5a5a5a5a5a5a5ull);
  const auto Q = transfer(false, Handle);
  ASSERT_NE(Q.IRP, 0u);
  const auto System = read(Q.IRP + IRPSystemBufferOffset);
  write(System, 0x1122334455667788);
  write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
  take(Model->finishGuestCall(Q.Call.Token, StatusPending));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  Model->enterExecution(profile::CallbackStackBase);
  complete(Q.IRP, 0, 8);
  EXPECT_EQ(read(Scratch + 0x400), 0xa5a5a5a5a5a5a5a5ull);
  EXPECT_EQ(read(Scratch + 0x500), 0xa5a5a5a5a5a5a5a5ull);
  ok(Model->validateGuestAccess(System, 8, false));
  EXPECT_TRUE(Result.Requests.back().Output.empty());
  auto Wrong = Model->resumeFileRequest(Q.ID);
  ASSERT_FALSE(bool(Wrong));
  rejects(Wrong.takeError(), "original caller");
  Model->enterExecution(profile::StackBase);
  call("KeEnterGuardedRegion", {});
  ok(Model->deliverFileRequestCompletions());
  EXPECT_EQ(read(Scratch + 0x400), 0xa5a5a5a5a5a5a5a5ull);
  call("KeLeaveGuardedRegion", {});
  EXPECT_EQ(read(Scratch + 0x400), 0x1122334455667788u);
  EXPECT_EQ(read(Scratch + 0x500, 4), 0u);
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
  call("ZwClose", {Handle});
  close();
}

TEST_F(KernelFileTransfers, UnsupportedShapesFailBeforePacketPublication) {
  const auto File = open(), Handle = handle(File);
  const auto Freed = call("ExAllocatePoolWithTag", {0, 32, 0x11223344});
  call("ExFreePoolWithTag", {Freed, 0x11223344});
  const std::array<uint64_t, 9> Base{Handle,          0,  0, 0, Scratch + 0x500,
                                     Scratch + 0x400, 16, 0, 0};
  for (const char *Name : {"ZwReadFile", "ZwWriteFile"}) {
    const auto Before = Result.Requests.size();
    for (unsigned Index : {1, 2, 3, 4, 5, 6, 7, 8}) {
      auto A = Base;
      A[Index] = Index == 6 ? profile::KernelArenaSize + 1 : Freed;
      auto Rejected = Model->call(Name, A);
      ASSERT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      EXPECT_EQ(Result.Requests.size(), Before);
      EXPECT_FALSE(Model->takeGuestCall());
      EXPECT_FALSE(Model->takeFileRequestContinuation());
    }
    for (uint64_t Flags : {uint64_t{0}, DeviceDirectIO}) {
      write(Device + DeviceFlagsOffset, Flags, 4);
      auto Rejected = Model->call(Name, Base);
      ASSERT_FALSE(bool(Rejected));
      rejects(Rejected.takeError(), "buffered");
    }
    write(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    auto A = Base;
    A[0] = 0xdeadbeef;
    EXPECT_EQ(take(Model->call(Name, A)), StatusInvalidHandle);
    auto IO = begin(DriverRequestKind::DeviceControl);
    auto Rejected = Model->call(Name, Base);
    ASSERT_FALSE(bool(Rejected));
    rejects(Rejected.takeError(), "same-file");
    finish(IO.IRP);
  }
  call("ZwClose", {Handle});
  close();
  const auto Async = open(true), AsyncHandle = handle(Async);
  for (const char *Name : {"ZwReadFile", "ZwWriteFile"}) {
    auto A = Base;
    A[0] = AsyncHandle;
    auto Rejected = Model->call(Name, A);
    ASSERT_FALSE(bool(Rejected));
    rejects(Rejected.takeError(), "synchronous file");
  }
  call("ZwClose", {AsyncHandle});
  close();
}

TEST_F(KernelFileTransfers, PositionFastPathMatchesLengthAlignmentAndAliases) {
  const auto File = open(), Handle = handle(File);
  const auto Before = Result.Requests.size();
  const auto Flags = read(File + FileFlagsOffset, 4);
  for (bool NoCache : {false, true})
    for (uint64_t Sector : std::array<uint64_t, 3>{0, 512, 4096})
      for (uint64_t Length : std::array<uint64_t, 5>{0, 7, 8, 16, 0x100000008})
        for (uint64_t Position :
             std::array<uint64_t, 5>{0, 1, 512, UINT64_MAX, INT64_MAX})
          for (bool Alias : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << NoCache << ' ' << Sector << ' ' << Length << ' '
                         << Position << ' ' << Alias);
            constexpr uint64_t IOSB = Scratch + 0x500, Old = 0x1122334455;
            const uint64_t Input = Alias ? IOSB : Scratch + 0x600;
            ok(Memory->writeInteger(File + FileFlagsOffset,
                                    Flags | (NoCache ? 8 : 0), 4));
            ok(Memory->writeInteger(Device + DeviceSectorSizeOffset, Sector,
                                    2));
            ok(Memory->writeInteger(File + FileCurrentByteOffset, Old, 8));
            ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
            write(Input, Position);
            std::array<uint8_t, 16> Expected, Observed;
            ok(Memory->read(IOSB, Expected));
            const uint32_t Status =
                uint32_t(Length) < 8 ? StatusInfoLengthMismatch
                : Position > INT64_MAX ||
                        (NoCache && Sector && (Position & (Sector - 1)))
                    ? StatusInvalidParameter
                    : 0;
            EXPECT_EQ(call("ZwSetInformationFile",
                           {Handle, IOSB, Input, Length, 0x10000000e}),
                      Status);
            if (!Status) {
              llvm::support::endian::write32le(Expected.data(), 0);
              llvm::support::endian::write64le(Expected.data() + 8, 0);
            }
            ok(Memory->read(IOSB, Observed));
            EXPECT_EQ(Observed, Expected);
            EXPECT_EQ(read(File + FileCurrentByteOffset),
                      Status ? Old : Position);
            EXPECT_EQ(Result.Requests.size(), Before);
            EXPECT_FALSE(Model->takeGuestCall());
            EXPECT_FALSE(Model->takeFileRequestContinuation());
            EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
            ASSERT_FALSE(HasFailure());
          }
  ok(Memory->writeInteger(File + FileFlagsOffset, Flags, 4));
  call("ZwClose", {Handle});
  close();
}

TEST_F(KernelFileTransfers, PositionStorePrecedesIOSBFaultAndFeedsReadOffset) {
  const auto File = open(), Handle = handle(File);
  write(Scratch + 0x600, 0x123456789);
  write(Scratch + 0xfff8, 0xa5a5a5a5a5a5a5a5ull);
  auto Fault = Model->call("ZwSetInformationFile",
                           {Handle, Scratch + 0xfff8, Scratch + 0x600, 8, 14});
  ASSERT_FALSE(bool(Fault));
  llvm::consumeError(Fault.takeError());
  EXPECT_EQ(read(File + FileCurrentByteOffset), 0x123456789u);
  EXPECT_EQ(read(Scratch + 0xfff8), 0xa5a5a5a500000000ull);
  EXPECT_EQ(call("ZwQueryInformationFile",
                 {Handle, Scratch + 0x500, Scratch + 0x400, 8, 14}),
            0u);
  EXPECT_EQ(read(Scratch + 0x400), 0x123456789u);
  auto Q = transfer(false, Handle, 0, 0, Scratch + 0x500, 0, 0);
  ASSERT_NE(Q.IRP, 0u);
  EXPECT_EQ(
      read(read(Q.IRP + IRPStackPointerOffset) + StackReadWriteByteOffset),
      0x123456789u);
  complete(Q.IRP);
  take(Model->finishGuestCall(Q.Call.Token, 0));
  EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
  EXPECT_EQ(read(File + FileCurrentByteOffset), 0x123456789u);
  call("ZwClose", {Handle});
  close();
}

TEST_F(KernelFileTransfers,
       PositionRequiresValidHandleAndSerialSynchronousFile) {
  const auto File = open(), Handle = handle(File);
  const auto Other = handle(call("PsGetCurrentProcess", {}));
  const std::array<uint64_t, 5> Base{Handle, Scratch + 0x500, Scratch + 0x600,
                                     8, 14};
  for (auto [Value, Status] :
       {std::pair{uint64_t{0xdeadbeef}, uint64_t{StatusInvalidHandle}},
        std::pair{Other, uint64_t{StatusObjectTypeMismatch}}}) {
    auto A = Base;
    A[0] = Value;
    EXPECT_EQ(take(Model->call("ZwSetInformationFile", A)), Status);
  }
  auto A = Base;
  A[4] = 4;
  auto Class = Model->call("ZwSetInformationFile", A);
  ASSERT_FALSE(bool(Class));
  rejects(Class.takeError(), "class is not implemented");
  auto IO = begin(DriverRequestKind::DeviceControl);
  auto Overlap = Model->call("ZwSetInformationFile", Base);
  ASSERT_FALSE(bool(Overlap));
  rejects(Overlap.takeError(), "same-file");
  finish(IO.IRP);
  call("KeEnterGuardedRegion", {});
  auto Disabled = Model->call("ZwSetInformationFile", Base);
  ASSERT_FALSE(bool(Disabled));
  rejects(Disabled.takeError(), "special kernel APCs");
  call("KeLeaveGuardedRegion", {});
  call("ZwClose", {Other});
  call("ZwClose", {Handle});
  close();
  const auto Async = open(true), AsyncHandle = handle(Async);
  A = Base;
  A[0] = AsyncHandle;
  auto Unsupported = Model->call("ZwSetInformationFile", A);
  ASSERT_FALSE(bool(Unsupported));
  rejects(Unsupported.takeError(), "asynchronous position");
  call("ZwClose", {AsyncHandle});
  close();
}

TEST_F(KernelFileTransfers, FlushChecksGrantedMaskInKernelMode) {
  for (bool Pipe : {false, true})
    for (uint32_t Access : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 0x100000u}) {
      SCOPED_TRACE(::testing::Message() << Pipe << ' ' << Access);
      const auto File = open(), Handle = handle(File, Access);
      const auto Flags = read(File + FileFlagsOffset, 4);
      ok(Memory->writeInteger(File + FileFlagsOffset, Flags | (Pipe ? 0x80 : 0),
                              4));
      const auto Before = Result.Requests.size();
      if (!(Access & (Pipe ? 2 : 6))) {
        EXPECT_EQ(call("ZwFlushBuffersFile", {Handle, Scratch + 0x500}),
                  StatusAccessDenied);
        EXPECT_EQ(Result.Requests.size(), Before);
        EXPECT_FALSE(Model->takeGuestCall());
        EXPECT_FALSE(Model->takeFileRequestContinuation());
      } else {
        const auto Q = flush(Handle);
        ASSERT_NE(Q.IRP, 0u);
        complete(Q.IRP);
        take(Model->finishGuestCall(Q.Call.Token, 0));
        EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                  std::optional<uint64_t>{0});
      }
      ok(Memory->writeInteger(File + FileFlagsOffset, Flags, 4));
      call("ZwClose", {Handle});
      close();
      ASSERT_FALSE(HasFailure());
    }
}

TEST_F(KernelFileTransfers, FlushPublishesAndDisposesBeforeDispatchReturns) {
  for (uint64_t DeviceFlags : {uint64_t{0}, DeviceBufferedIO, DeviceDirectIO})
    for (bool Pending : {false, true})
      for (uint32_t Status : {0u, 0x80000005u, 0xc0000001u}) {
        SCOPED_TRACE(::testing::Message()
                     << DeviceFlags << ' ' << Pending << ' ' << Status);
        const auto File = open(), Handle = handle(File, 2);
        constexpr uint64_t IOSB = Scratch + 0x500, Information = 0x100000007;
        const auto FileFlags = read(File + FileFlagsOffset, 4);
        write(Device + DeviceFlagsOffset, DeviceFlags, 4);
        ok(Memory->writeInteger(File + FileFlagsOffset, FileFlags | 0x18, 4));
        ok(Memory->writeInteger(File + FileCurrentByteOffset, 0x123456789, 8));
        ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
        const auto Q = flush(Handle);
        ASSERT_NE(Q.IRP, 0u);
        const auto Stack = read(Q.IRP + IRPStackPointerOffset);
        EXPECT_EQ(read(Stack, 1), 9u);
        EXPECT_EQ(read(Stack + StackMinorOffset, 1), 0u);
        EXPECT_EQ(read(Stack + StackFileOffset), File);
        EXPECT_EQ(read(Q.IRP + IRPFlagsOffset, 4), 0x60000u);
        EXPECT_EQ(read(Q.IRP + IRPAllocationFlagsOffset, 1), 2u);
        EXPECT_EQ(read(Q.IRP + IRPSystemBufferOffset), 0u);
        EXPECT_EQ(read(Q.IRP + IRPUserBufferOffset), 0u);
        EXPECT_EQ(read(Q.IRP + IRPUserEventOffset), 0u);
        EXPECT_EQ(read(Q.IRP + IRPUserIosbOffset), IOSB);
        if (Pending)
          write(Stack + StackControlOffset, 1, 1);
        complete(Q.IRP, Status, Information);
        // The target consumes this nondeferred packet inside completion.
        rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
        const bool Published = !ntError(Status) || Pending;
        EXPECT_EQ(read(IOSB, 4), Published ? Status : 0xa5a5a5a5u);
        EXPECT_EQ(read(IOSB + 4, 4), 0xa5a5a5a5u);
        EXPECT_EQ(read(IOSB + 8),
                  Published ? Information : 0xa5a5a5a5a5a5a5a5ull);
        // A nonpending dispatch result remains independent of its IOSB.
        take(Model->finishGuestCall(Q.Call.Token, Pending ? StatusPending : 0));
        EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                  std::optional<uint64_t>{Pending ? Status : 0});
        const auto &ResultRequest = Result.Requests.back();
        EXPECT_EQ(ResultRequest.Kind, DriverRequestKind::FlushBuffers);
        EXPECT_EQ(ResultRequest.Origin, DriverRequestOrigin::FileHandleFlush);
        EXPECT_EQ(ResultRequest.Information, Information);
        EXPECT_TRUE(ResultRequest.Output.empty());
        EXPECT_FALSE(ResultRequest.FileInformationClass);
        EXPECT_EQ(read(File + FileCurrentByteOffset), 0x123456789u);
        ok(Memory->writeInteger(File + FileFlagsOffset, FileFlags, 4));
        call("ZwClose", {Handle});
        close();
        ASSERT_FALSE(HasFailure());
      }
}

TEST_F(KernelFileTransfers, FlushWaitsForEligibleCallerNotification) {
  for (bool Worker : {false, true}) {
    const auto File = open(), Handle = handle(File, 2);
    constexpr uint64_t IOSB = Scratch + 0x500, Information = 0x100000007;
    ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
    const auto Q = flush(Handle);
    ASSERT_NE(Q.IRP, 0u);
    if (Worker) {
      write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
      take(Model->finishGuestCall(Q.Call.Token, StatusPending));
      ASSERT_TRUE(Model->takeWait());
      Model->enterExecution(profile::CallbackStackBase);
    } else {
      call("KeEnterGuardedRegion", {});
    }
    complete(Q.IRP, 0, Information);
    EXPECT_EQ(read(IOSB), 0xa5a5a5a5a5a5a5a5ull);
    ok(Model->validateGuestAccess(Q.IRP + IRPFlagsOffset, 4, false));
    auto Immutable =
        Model->validateGuestAccess(Q.IRP + IRPStatusOffset, 4, true);
    ASSERT_TRUE(bool(Immutable));
    llvm::consumeError(std::move(Immutable));
    if (Worker) {
      Model->enterExecution(profile::StackBase);
      ok(Model->deliverFileRequestCompletions());
    } else {
      call("KeLeaveGuardedRegion", {});
    }
    EXPECT_EQ(read(IOSB, 4), 0u);
    EXPECT_EQ(read(IOSB + 8), Information);
    rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
    if (!Worker)
      take(Model->finishGuestCall(Q.Call.Token, 0));
    EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
    call("ZwClose", {Handle});
    close();
    ASSERT_FALSE(HasFailure());
  }
}

TEST_F(KernelFileTransfers, FlushRejectsAsyncAndConcurrentRequests) {
  const auto File = open(), Handle = handle(File, 2);
  auto IO = begin(DriverRequestKind::DeviceControl);
  const auto Before = Result.Requests.size();
  auto Busy = Model->call("ZwFlushBuffersFile", {Handle, Scratch + 0x500});
  ASSERT_FALSE(bool(Busy));
  rejects(Busy.takeError(), "same-file");
  EXPECT_EQ(Result.Requests.size(), Before);
  finish(IO.IRP);
  call("ZwClose", {Handle});
  close();
  const auto AsyncFile = open(true), AsyncHandle = handle(AsyncFile, 2);
  auto Async =
      Model->call("ZwFlushBuffersFile", {AsyncHandle, Scratch + 0x500});
  ASSERT_FALSE(bool(Async));
  rejects(Async.takeError(), "synchronous file");
  EXPECT_FALSE(Model->takeFileRequestContinuation());
  EXPECT_FALSE(Model->takeGuestCall());
  call("ZwClose", {AsyncHandle});
  close();
}

TEST_F(KernelFileTransfers, ControlPacketsCaptureLengthsAccessAndInput) {
  for (bool FS : {false, true})
    for (uint64_t DeviceFlags : {uint64_t{0}, DeviceBufferedIO, DeviceDirectIO})
      for (uint32_t Access : {0u, 1u, 2u, 3u})
        for (auto Sizes :
             {std::pair{0u, 0u}, {0u, 16u}, {16u, 0u}, {5u, 16u}, {16u, 5u}}) {
          SCOPED_TRACE(::testing::Message()
                       << FS << ' ' << DeviceFlags << ' ' << Access << ' '
                       << Sizes.first << ' ' << Sizes.second);
          const auto File = open(), Handle = handle(File, Access);
          const auto [Input, Output] = Sizes;
          const auto Length = std::max(Input, Output);
          write(Device + DeviceFlagsOffset, DeviceFlags, 4);
          std::vector<uint8_t> Bytes(16);
          for (unsigned I = 0; I < Bytes.size(); ++I)
            Bytes[I] = I + 1;
          ok(Memory->write(Scratch + 0x400, Bytes));
          const auto Q =
              control(FS, Handle, 0xaabbccdd00000000ull | Input,
                      0xaabbccdd00000000ull | Output, 0xaabbccdd0022e000ull);
          ASSERT_NE(Q.IRP, 0u);
          const auto Stack = read(Q.IRP + IRPStackPointerOffset);
          const auto Buffer = read(Q.IRP + IRPSystemBufferOffset);
          EXPECT_EQ(read(Stack, 1), FS ? 13u : 14u);
          EXPECT_EQ(read(Stack + StackMinorOffset, 1), 0u);
          EXPECT_EQ(read(Stack + StackFlagsOffset, 1),
                    (Access & 1) | (2 * (Access & 2)));
          EXPECT_EQ(read(Stack + StackParametersOffset, 4), Output);
          EXPECT_EQ(read(Stack + StackInputLengthOffset, 4), Input);
          EXPECT_EQ(read(Stack + StackIOControlOffset, 4), 0x22e000u);
          EXPECT_EQ(read(Stack + StackType3InputOffset), 0u);
          EXPECT_EQ(read(Q.IRP + IRPMdlOffset), 0u);
          EXPECT_EQ(read(Q.IRP + IRPUserEventOffset), 0u);
          EXPECT_EQ(read(Q.IRP + IRPUserBufferOffset),
                    Length ? Scratch + 0x480 : 0u);
          EXPECT_EQ(read(Q.IRP + IRPFlagsOffset, 4),
                    0x60000u | (FS ? 0x800u : 0u) |
                        (Length ? (Output ? 0x70u : 0x30u) : 0u));
          EXPECT_EQ(read(Q.IRP + IRPAllocationFlagsOffset, 1), 2u);
          EXPECT_EQ(Buffer != 0, Length != 0);
          for (unsigned I = 0; I < Length; ++I)
            EXPECT_EQ(read(Buffer + I, 1), I < Input ? Bytes[I] : 0u);
          // Captured input is independent of subsequent caller mutations.
          ok(Memory->writeInteger(Scratch + 0x400, 0xff, 1));
          if (Input)
            EXPECT_EQ(read(Buffer, 1), 1u);
          complete(Q.IRP);
          take(Model->finishGuestCall(Q.Call.Token, 0));
          EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                    std::optional<uint64_t>{0});
          call("ZwClose", {Handle});
          close();
          ASSERT_FALSE(HasFailure());
        }
}

TEST_F(KernelFileTransfers, ControlCompletionMatchesProducerDeferral) {
  for (bool FS : {false, true})
    for (bool Pending : {false, true})
      for (uint32_t Status : {0u, 0x80000005u, 0xc0000001u})
        for (uint32_t Output : {0u, 16u}) {
          SCOPED_TRACE(::testing::Message() << FS << ' ' << Pending << ' '
                                            << Status << ' ' << Output);
          const auto File = open(), Handle = handle(File);
          constexpr uint64_t IOSB = Scratch + 0x500;
          const uint64_t Information = Output ? 7 : 0x100000007ull;
          ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
          ok(Memory->write(Scratch + 0x480, std::vector<uint8_t>(16, 0x5a)));
          ok(Memory->writeInteger(File + FileCurrentByteOffset, 0x123456789,
                                  8));
          const auto Q = control(FS, Handle, 16, Output);
          ASSERT_NE(Q.IRP, 0u);
          const auto Buffer = read(Q.IRP + IRPSystemBufferOffset);
          write(Buffer, 0x26252423222120, 8);
          if (Pending)
            write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1,
                  1);
          complete(Q.IRP, Status, Information);
          if (FS && !Pending) {
            ok(Model->validateGuestAccess(Q.IRP + IRPFlagsOffset, 4, false));
            EXPECT_EQ(read(IOSB), 0xa5a5a5a5a5a5a5a5ull);
            EXPECT_EQ(read(Scratch + 0x480), 0x5a5a5a5a5a5a5a5aull);
          } else {
            rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
          }
          take(Model->finishGuestCall(Q.Call.Token,
                                      Pending ? StatusPending : 0));
          EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                    std::optional<uint64_t>{Pending ? Status : 0});
          const bool Published = !ntError(Status) || Pending;
          EXPECT_EQ(read(IOSB, 4), Published ? Status : 0xa5a5a5a5u);
          EXPECT_EQ(read(IOSB + 4, 4), 0xa5a5a5a5u);
          EXPECT_EQ(read(IOSB + 8),
                    Published ? Information : 0xa5a5a5a5a5a5a5a5ull);
          const auto &Request = Result.Requests.back();
          EXPECT_EQ(Request.Kind, FS ? DriverRequestKind::FileSystemControl
                                     : DriverRequestKind::DeviceControl);
          EXPECT_EQ(Request.Origin,
                    FS ? DriverRequestOrigin::FileHandleSystemControl
                       : DriverRequestOrigin::FileHandleDeviceControl);
          EXPECT_EQ(Request.ControlCode, 0x22e000u);
          EXPECT_EQ(Request.Information, Information);
          EXPECT_FALSE(Request.FileInformationClass);
          EXPECT_EQ(Request.Output,
                    Output && !ntError(Status)
                        ? std::vector<uint8_t>(
                              {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26})
                        : std::vector<uint8_t>{});
          EXPECT_EQ(read(Scratch + 0x480), Output && !ntError(Status)
                                               ? 0x5a26252423222120ull
                                               : 0x5a5a5a5a5a5a5a5aull);
          EXPECT_EQ(read(File + FileCurrentByteOffset), 0x123456789u);
          rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
          rejects(Model->validateGuestAccess(Buffer, 1, false), "freed");
          call("ZwClose", {Handle});
          close();
          ASSERT_FALSE(HasFailure());
        }
}

TEST_F(KernelFileTransfers, ControlCopiesReadOnlyInputBeforeIOSBAliases) {
  for (bool FS : {false, true}) {
    const auto File = open(), Handle = handle(File);
    constexpr uint64_t Input = Scratch + 0x2000, IOSB = Scratch + 0x500;
    std::vector<uint8_t> Bytes{0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
    ok(Memory->write(Input, Bytes));
    ok(Memory->protect(Input, 0x1000, Read));
    ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
    const auto Q = control(FS, Handle, 7, 16, 0x22e000, Input, IOSB, IOSB);
    ASSERT_NE(Q.IRP, 0u);
    complete(Q.IRP, 0, 7);
    take(Model->finishGuestCall(Q.Call.Token, 0));
    EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
    EXPECT_EQ(read(IOSB, 4), 0u);
    EXPECT_EQ(read(IOSB + 4, 4), 0xa5262524u);
    EXPECT_EQ(read(IOSB + 8), 7u);
    EXPECT_EQ(Result.Requests.back().Output, Bytes);
    EXPECT_EQ(read(Input, 4), 0x23222120u);
    ok(Memory->protect(Input, 0x1000, Read | Write));
    call("ZwClose", {Handle});
    close();
    ASSERT_FALSE(HasFailure());
  }
}

TEST_F(KernelFileTransfers, ControlWaitsForEligibleCallerNotification) {
  for (bool FS : {false, true})
    for (unsigned Context : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::Message() << FS << ' ' << Context);
      const auto File = open(), Handle = handle(File);
      constexpr uint64_t IOSB = Scratch + 0x500;
      ok(Memory->write(IOSB, std::vector<uint8_t>(16, 0xa5)));
      ok(Memory->write(Scratch + 0x480, std::vector<uint8_t>(16, 0x5a)));
      const auto Q = control(FS, Handle);
      ASSERT_NE(Q.IRP, 0u);
      write(read(Q.IRP + IRPSystemBufferOffset), 0x26252423222120, 8);
      write(read(Q.IRP + IRPStackPointerOffset) + StackControlOffset, 1, 1);
      uint64_t OldIRQL = 0;
      if (Context == 0) {
        take(Model->finishGuestCall(Q.Call.Token, StatusPending));
        ASSERT_TRUE(Model->takeWait());
        Model->enterExecution(profile::CallbackStackBase);
      } else if (Context == 1) {
        call("KeEnterGuardedRegion", {});
      } else {
        OldIRQL = call("KfRaiseIrql", {1});
      }
      complete(Q.IRP, 0, 7);
      EXPECT_EQ(read(IOSB), 0xa5a5a5a5a5a5a5a5ull);
      EXPECT_EQ(read(Scratch + 0x480), 0x5a5a5a5a5a5a5a5aull);
      ok(Model->validateGuestAccess(Q.IRP + IRPFlagsOffset, 4, false));
      if (Context == 0) {
        Model->enterExecution(profile::StackBase);
        ok(Model->deliverFileRequestCompletions());
      } else if (Context == 1) {
        call("KeLeaveGuardedRegion", {});
      } else {
        call("KeLowerIrql", {OldIRQL});
      }
      EXPECT_EQ(read(IOSB, 4), 0u);
      EXPECT_EQ(read(IOSB + 8), 7u);
      EXPECT_EQ(read(Scratch + 0x480), 0x5a26252423222120ull);
      rejects(Model->validateGuestAccess(Q.IRP, 1, false), "freed");
      if (Context)
        take(Model->finishGuestCall(Q.Call.Token, StatusPending));
      EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)),
                std::optional<uint64_t>{0});
      call("ZwClose", {Handle});
      close();
      ASSERT_FALSE(HasFailure());
    }
}

TEST_F(KernelFileTransfers, ControlOutputBoundsDoNotUseLargerInputSize) {
  for (bool FS : {false, true}) {
    const auto File = open(), Handle = handle(File);
    const auto Q = control(FS, Handle, 64, 4);
    ASSERT_NE(Q.IRP, 0u);
    write(Q.IRP + IRPStatusOffset, 0, 4);
    write(Q.IRP + IRPInformationOffset, 5);
    auto Rejected = Model->call("IofCompleteRequest", {Q.IRP, 0});
    ASSERT_FALSE(bool(Rejected));
    rejects(Rejected.takeError(), "requested transfer buffer");
    EXPECT_FALSE(Result.Requests.back().Completed);
    complete(Q.IRP, 0, 4);
    take(Model->finishGuestCall(Q.Call.Token, 0));
    EXPECT_EQ(take(Model->resumeFileRequest(Q.ID)), std::optional<uint64_t>{0});
    call("ZwClose", {Handle});
    close();
  }
}

TEST_F(KernelFileTransfers, ControlRefusesUnmodeledShapesBeforePublication) {
  const auto File = open(), Handle = handle(File);
  const auto Freed = call("ExAllocatePoolWithTag", {0, 32, 0x11223344});
  call("ExFreePoolWithTag", {Freed, 0x11223344});
  const std::array<uint64_t, 10> Base{
      Handle,          0, 0, 0, Scratch + 0x500, 0x22e000, Scratch + 0x400, 16,
      Scratch + 0x480, 16};
  for (const char *Name : {"ZwDeviceIoControlFile", "ZwFsControlFile"}) {
    const auto Before = Result.Requests.size();
    for (unsigned Index : {1, 2, 3, 4, 6, 7, 8, 9}) {
      auto A = Base;
      A[Index] =
          Index == 7 || Index == 9 ? profile::KernelArenaSize + 1 : Freed;
      auto Rejected = Model->call(Name, A);
      ASSERT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      EXPECT_EQ(Result.Requests.size(), Before);
      EXPECT_FALSE(Model->takeGuestCall());
      EXPECT_FALSE(Model->takeFileRequestContinuation());
    }
    for (uint32_t Code : {0x22e001u, 0x22e002u, 0x22e003u, 0x94264u, 0x98268u,
                          0x9042cu, 0x90020u}) {
      auto A = Base;
      A[5] = Code;
      auto Rejected = Model->call(Name, A);
      ASSERT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      EXPECT_EQ(Result.Requests.size(), Before);
    }
    auto A = Base;
    A[6] = 0;
    auto Missing = Model->call(Name, A);
    ASSERT_FALSE(bool(Missing));
    rejects(Missing.takeError(), "initialized input");
    write(Device + DeviceFlagsOffset, 0x80000, 4);
    auto Bypass = Model->call(Name, Base);
    ASSERT_FALSE(bool(Bypass));
    rejects(Bypass.takeError(), "bypass");
    write(Device + DeviceFlagsOffset, DeviceBufferedIO, 4);
    auto IO = begin(DriverRequestKind::DeviceControl);
    auto Busy = Model->call(Name, Base);
    ASSERT_FALSE(bool(Busy));
    rejects(Busy.takeError(), "same-file");
    finish(IO.IRP);
  }
  write(Model->driverObject() + DriverFastIOOffset, Scratch + 0x1000);
  auto Fast = Model->call("ZwDeviceIoControlFile", Base);
  ASSERT_FALSE(bool(Fast));
  rejects(Fast.takeError(), "Fast I/O");
  write(Model->driverObject() + DriverFastIOOffset, 0);
  call("ZwClose", {Handle});
  close();
  const auto AsyncFile = open(true), AsyncHandle = handle(AsyncFile);
  for (const char *Name : {"ZwDeviceIoControlFile", "ZwFsControlFile"}) {
    auto A = Base;
    A[0] = AsyncHandle;
    auto Async = Model->call(Name, A);
    ASSERT_FALSE(bool(Async));
    rejects(Async.takeError(), "synchronous file");
  }
  call("ZwClose", {AsyncHandle});
  close();
}
} // namespace
} // namespace neverd::emulation
