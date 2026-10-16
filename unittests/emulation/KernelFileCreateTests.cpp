//===- KernelFileCreateTests.cpp - Driver-owned CREATE lifetime -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise real provider packets, caller publication and final file release.

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"
#include "os/windows/kernel/WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;
class KernelFileCreate : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Entry = 0x180001000;
  static constexpr uint64_t HandleOut = Scratch + 0x100;
  static constexpr uint64_t StatusOut = Scratch + 0x120;
  std::shared_ptr<AddressSpace> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t Volume = 0, Filter = 0;
  void ok(llvm::Error E) {
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
  void rejects(llvm::Error E, llvm::StringRef Fragment) {
    ASSERT_TRUE(bool(E));
    auto Text = llvm::toString(std::move(E));
    EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
  }
  template <class T>
  void rejects(llvm::Expected<T> Value, llvm::StringRef Fragment) {
    ASSERT_FALSE(bool(Value));
    rejects(Value.takeError(), Fragment);
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Arguments) {
    return take(Model->call(Name, Arguments));
  }
  void write(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model->validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t read(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  static DriverFileSystemNode node(std::string Path, uint64_t ID,
                                   bool Directory = false) {
    DriverFileSystemNode Node;
    Node.Path = std::move(Path);
    Node.Kind =
        Directory ? DriverFileNodeKind::Directory : DriverFileNodeKind::File;
    Node.FileID = ID;
    Node.Attributes = Directory ? 0x10 : 0x80;
    Node.AllocationSize = 0;
    Node.CreationTime100ns = Node.LastAccessTime100ns =
        Node.LastWriteTime100ns = Node.ChangeTime100ns = 0;
    if (!Directory)
      Node.Data.emplace();
    return Node;
  }
  void SetUp() override {
    Memory = take(AddressSpace::create(
        take(PhysicalMemory::create(16 * 1024 * 1024)), 16 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    ok(Memory->map(0x180000000, 0x3000, Read | Write));
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverOptions Options;
    DriverFileSystemVolume V;
    V.DeviceName = "\\Device\\OpenVolume";
    V.AccessPolicy = DriverFileAccessPolicy::FullAccess;
    V.SectorSize = 512;
    V.AlignmentMask = 0;
    V.Nodes = {node("\\", 0, true), node("\\Data", 1),
               node("\\Folder", 2, true), node("\\Folder\\Child", 3),
               node("\\ReadOnly", 4)};
    V.Nodes.back().Attributes = 1;
    Options.FileSystem = std::vector{V};
    Result.Configuration = Options;
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    Model->enterExecution(profile::StackBase);
    ok(Model->snapshot());
    Volume = Result.FileSystem->front().DeviceObject;
  }
  void name(std::string Text, uint64_t Root = 0,
            uint32_t Attributes = ObjectCaseInsensitive | ObjectKernelHandle) {
    std::vector<uint8_t> Bytes(Text.size() * 2);
    for (size_t I = 0; I != Text.size(); ++I)
      Bytes[I * 2] = uint8_t(Text[I]);
    ok(Memory->write(Scratch + 0x200, Bytes));
    write(Scratch + 0x80, Bytes.size(), 2);
    write(Scratch + 0x82, Bytes.size(), 2);
    write(Scratch + 0x88, Scratch + 0x200);
    write(Scratch, ObjectAttributesSize, 4);
    write(Scratch + ObjectRootOffset, Root);
    write(Scratch + ObjectNameOffset, Scratch + 0x80);
    write(Scratch + ObjectFlagsOffset, Attributes, 4);
    write(Scratch + ObjectSecurityOffset, 0);
    write(Scratch + ObjectQualityOffset, 0);
  }
  KernelModel::FileAPIContinuation start(uint32_t Access = 0x100081,
                                         uint32_t Share = 7,
                                         uint32_t Options = 0x60,
                                         uint64_t Output = HandleOut) {
    call("ZwOpenFile", {Output, Access, Scratch, StatusOut, Share, Options});
    auto Pending = Model->takeFileAPIContinuation();
    EXPECT_TRUE(Pending);
    return Pending.value_or(KernelModel::FileAPIContinuation{});
  }
  uint64_t finish(KernelModel::FileAPIContinuation Operation) {
    auto Status = take(Model->resumeFileAPI(Operation));
    EXPECT_TRUE(Status);
    return Status.value_or(UINT64_MAX);
  }
  uint64_t open(std::string Path = "\\Device\\OpenVolume\\Data",
                uint32_t Access = 0x100081, uint32_t Share = 7,
                uint32_t Options = 0x60) {
    name(std::move(Path));
    EXPECT_EQ(finish(start(Access, Share, Options)), 0u);
    return read(HandleOut);
  }
  void attach() {
    call("IoCreateDevice",
         {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch + 0x180});
    Filter = read(Scratch + 0x180);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {Filter, Volume}), Volume);
    for (unsigned Major : {0, 2, 18})
      write(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
  }
  KernelGuestCall guest() {
    auto Call = Model->takeGuestCall();
    EXPECT_TRUE(Call);
    if (!Call)
      return {};
    ok(Model->beginGuestCall(Call->Token));
    return *Call;
  }
  uint64_t forward(uint64_t IRP, uint64_t Completion = 0) {
    const auto Stack = read(IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackSize> Bytes;
    ok(Memory->read(Stack, Bytes));
    ok(Model->validateGuestAccess(Stack - StackSize, StackSize, true));
    ok(Memory->write(Stack - StackSize, Bytes));
    write(Stack - StackSize + StackControlOffset, Completion ? 0xe0 : 0, 1);
    write(Stack - StackSize + StackCompletionOffset, Completion);
    return call("IofCallDriver", {Volume, IRP});
  }
};

TEST_F(KernelFileCreate, IndependentBodiesShareNodesAndCloseWithoutScenario) {
  const auto First = open();
  const auto File = *Result.Requests.front().FileObject;
  const auto Second = open();
  const auto Other = *Result.Requests.back().FileObject;
  EXPECT_NE(First, Second);
  EXPECT_NE(File, Other);
  EXPECT_EQ(Result.Requests.front().FileSystemDevice, Volume);
  EXPECT_EQ(Result.Requests.front().FileSystemFileID, 1u);
  EXPECT_EQ(Result.Requests.front().Information, 1u);
  EXPECT_EQ(Result.Requests.front().Origin, DriverRequestOrigin::FileCreate);
  EXPECT_NE(Result.Requests.front().FileOpenOperation,
            Result.Requests.back().FileOpenOperation);
  EXPECT_EQ(read(File + FileFlagsOffset, 4) & FileHandleCreated,
            FileHandleCreated);
  EXPECT_EQ(call("ZwClose", {First}), 0u);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
  ok(Model->validateGuestAccess(Other, 1, false));
  EXPECT_EQ(call("ZwClose", {Second}), 0u);
  EXPECT_EQ(Result.Requests.size(), 6u);
  EXPECT_EQ(Result.Requests.back().Origin, DriverRequestOrigin::FileFinalClose);
  EXPECT_EQ(Result.Requests.back().FileObject, Other);
  ok(Model->snapshot());
}

TEST_F(KernelFileCreate, QueriesDeclaredMetadataThroughBothProducerContracts) {
  for (bool Native : {false, true}) {
    const auto Handle = open();
    const auto File = *Result.Requests.back().FileObject;
    const auto OpenID = Result.Requests.back().FileOpenOperation;
    const uint64_t Buffer = Scratch + 0x800, IOSB = Scratch + 0x900;
    for (auto [Class, Size] : {std::pair{4u, 40u},
                               {5u, 24u},
                               {6u, 8u},
                               {9u, 14u},
                               {34u, 56u},
                               {35u, 8u}}) {
      SCOPED_TRACE(::testing::Message() << Native << ' ' << Class);
      ok(Memory->write(Buffer, std::vector<uint8_t>(64, 0xa5)));
      if (Native)
        call("ZwQueryInformationFile", {Handle, IOSB, Buffer, 64, Class});
      else
        call("IoQueryFileInformation", {File, Class, 64, Buffer, IOSB});
      auto Pending = Model->takeFileAPIContinuation();
      ASSERT_TRUE(Pending);
      EXPECT_EQ(finish(*Pending), 0u);
      EXPECT_EQ(read(Native ? IOSB + 8 : IOSB, Native ? 8 : 4), Size);
      EXPECT_EQ(Result.Requests.back().FileOpenOperation, OpenID);
      EXPECT_EQ(Result.Requests.back().FileSystemFileID, 1u);
      EXPECT_EQ(Result.Requests.back().Output.size(), Size);
      EXPECT_EQ(read(Buffer + Size, 1), 0xa5u);
      if (Class == 4 || Class == 34)
        EXPECT_EQ(read(Buffer + (Class == 4 ? 32 : 48), 4), 0x80u);
      if (Class == 5)
        EXPECT_EQ(read(Buffer + 16, 4), 1u);
      if (Class == 6)
        EXPECT_EQ(read(Buffer), 1u);
      if (Class == 9) {
        EXPECT_EQ(read(Buffer, 4), 10u);
        EXPECT_EQ(read(Buffer + 4, 2), uint64_t('\\'));
      }
    }
    EXPECT_EQ(call("ZwClose", {Handle}), 0u);
    ok(Model->snapshot());
  }
}

TEST_F(KernelFileCreate, CleanupReleasesSharingBeforeLastPointerClose) {
  const auto Handle = open("\\Device\\OpenVolume\\Data", 0x100081, 0);
  const auto File = *Result.Requests.front().FileObject;
  call("ObfReferenceObject", {File});
  name("\\Device\\OpenVolume\\Data");
  write(HandleOut, 0x1122334455667788);
  write(StatusOut, 0x8877665544332211);
  EXPECT_EQ(finish(start()), StatusSharingViolation);
  EXPECT_EQ(read(HandleOut), 0x1122334455667788u);
  EXPECT_EQ(read(StatusOut), 0x8877665544332211u);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  EXPECT_EQ(Result.Requests.back().Kind, DriverRequestKind::Cleanup);
  EXPECT_TRUE(read(File + FileFlagsOffset, 4) & FileCleanupComplete);
  const auto Other = open("\\Device\\OpenVolume\\Data", 0x100006, 0);
  EXPECT_EQ(call("ZwClose", {Other}), 0u);
  call("ObfDereferenceObject", {File});
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
  ok(Model->snapshot());
}

TEST_F(KernelFileCreate, DirectoriesRelativeRootsAndFailureOutputs) {
  const auto Root = open("\\Device\\OpenVolume\\Folder", 0x100081, 7, 0x21);
  name("Child", Root);
  EXPECT_EQ(finish(start()), 0u);
  const auto Child = read(HandleOut);
  const auto &R = Result.Requests.back();
  EXPECT_EQ(R.FileSystemFileID, 3u);
  EXPECT_EQ(read(*R.FileObject + FileRelatedOffset),
            *Result.Requests.front().FileObject);
  EXPECT_EQ(call("ZwClose", {Root}), 0u);
  EXPECT_EQ(call("ZwClose", {Child}), 0u);
  // Read-only data forbids write/append access. Merely requesting DELETE is
  // distinct from the unsupported delete-on-close operation (MS-FSA 2.1.5.1).
  const auto DeleteAccess = open("\\Device\\OpenVolume\\ReadOnly", 0x110000);
  EXPECT_EQ(call("ZwClose", {DeleteAccess}), 0u);
  for (const auto &[Path, Access, Options, Status] :
       std::vector<std::tuple<std::string, uint32_t, uint32_t, uint32_t>>{
           {"\\Missing", 0x100081, 0x60, StatusObjectNameNotFound},
           {"\\Missing\\Child", 0x100081, 0x60, StatusObjectPathNotFound},
           {"\\Folder", 0x100081, 0x60, StatusFileIsADirectory},
           {"\\Data", 0x100081, 0x21, StatusNotADirectory},
           {"\\ReadOnly", 0x100002, 0x60, StatusAccessDenied}}) {
    SCOPED_TRACE(Path);
    name("\\Device\\OpenVolume" + Path);
    write(HandleOut, 0x1122334455667788);
    write(StatusOut, 0x8877665544332211);
    EXPECT_EQ(finish(start(Access, 7, Options)), Status);
    EXPECT_EQ(read(HandleOut), 0x1122334455667788u);
    EXPECT_EQ(read(StatusOut), 0x8877665544332211u);
  }
}

TEST_F(KernelFileCreate, FilterSeesOriginalPacketAndCannotForgeSuccess) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  const auto Operation = start();
  const auto Call = guest();
  ASSERT_NE(Call.Token.ID, 0u);
  const auto IRP = Call.Arguments[1];
  const auto Stack = read(IRP + IRPStackPointerOffset);
  const auto File = read(Stack + StackFileOffset);
  EXPECT_EQ(read(IRP + IRPFlagsOffset, 4), 0x884u);
  EXPECT_EQ(read(IRP + IRPOriginalFileOffset), File);
  EXPECT_EQ(read(IRP + IRPThreadOffset), call("PsGetCurrentThread", {}));
  EXPECT_EQ(read(Stack, 1), 0u);
  EXPECT_EQ(read(Stack + StackInputLengthOffset, 4), 0x1000060u);
  const auto Security = read(Stack + StackParametersOffset);
  EXPECT_EQ(read(Security + 0x10, 4), 0x100081u);
  rejects(Model->validateGuestAccess(read(Security + 8), 1, false), "opaque");
  write(IRP + IRPStatusOffset, 0, 4);
  write(IRP + IRPInformationOffset, 1);
  rejects(Model->call("IofCompleteRequest", {IRP, 0}), "terminal provider");
  EXPECT_EQ(forward(IRP), 0u);
  ok(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false));
  rejects(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, true),
          "read-only");
  rejects(Model->validateGuestAccess(IRP + IRPFlagsOffset, 4, true),
          "read-only");
  ok(Model->validateGuestAccess(Security + 0x10, 4, false));
  rejects(Model->validateGuestAccess(Security + 0x10, 4, true), "read-only");
  EXPECT_EQ(read(IRP + IRPStatusOffset, 4), 0u);
  EXPECT_EQ(read(IRP + IRPInformationOffset), 1u);
  EXPECT_EQ(call("KeReadStateEvent", {File + FileEventOffset}), 0u);
  take(Model->finishGuestCall(Call.Token, 0));
  EXPECT_EQ(finish(Operation), 0u);
  rejects(Model->validateGuestAccess(IRP, 1, false), "freed");
  EXPECT_EQ(call("KeReadStateEvent", {File + FileEventOffset}), 1u);
  EXPECT_EQ(Result.Requests.back().FileSystemFileID, 1u);
}

TEST_F(KernelFileCreate, CancelAfterProviderSuccessKeepsFailureAndFinalClose) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  write(HandleOut, 0x1122334455667788);
  write(StatusOut, 0x8877665544332211);
  const auto Operation = start(0x100081, 0);
  const auto Create = guest();
  const auto IRP = Create.Arguments[1];
  const auto File = read(IRP + IRPOriginalFileOffset);
  EXPECT_EQ(forward(IRP, Entry + 0x20), 0u);
  const auto Completion = guest();
  write(IRP + IRPStatusOffset, StatusAccessDenied, 4);
  write(IRP + IRPInformationOffset, 0);
  call("IoCancelFileOpen", {Filter, File});
  const auto Cancel = Model->takeFileAPIContinuation();
  ASSERT_TRUE(Cancel);
  const auto Cleanup = guest();
  const auto CleanupIRP = Cleanup.Arguments[1];
  const auto Event = read(CleanupIRP + IRPUserEventOffset);
  EXPECT_EQ(read(CleanupIRP + IRPFlagsOffset, 4), 0x404u);
  EXPECT_EQ(read(CleanupIRP + IRPRequestorModeOffset, 1), 0u);
  EXPECT_EQ(read(CleanupIRP + IRPOriginalFileOffset), File);
  EXPECT_EQ(read(CleanupIRP + IRPThreadOffset), call("PsGetCurrentThread", {}));
  EXPECT_EQ(read(CleanupIRP + IRPUserIosbOffset), CleanupIRP + IRPStatusOffset);
  EXPECT_NE(Event, 0u);
  const auto Stack = read(CleanupIRP + IRPStackPointerOffset);
  write(Stack + StackControlOffset, StackPendingReturned, 1);
  take(Model->finishGuestCall(Cleanup.Token, StatusPending));
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(profile::CallbackStackBase);
  EXPECT_EQ(forward(CleanupIRP), 0u);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  rejects(Model->resumeFileAPI(*Cancel), "original caller");
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(finish(*Cancel), 0u);
  EXPECT_TRUE(read(File + FileFlagsOffset, 4) & FileOpenCancelled);
  EXPECT_TRUE(read(File + FileFlagsOffset, 4) & FileCleanupComplete);
  if (Event)
    rejects(Model->validateGuestAccess(Event, 1, false), "freed");
  take(Model->finishGuestCall(Completion.Token, 0));
  take(Model->finishGuestCall(Create.Token, StatusAccessDenied));
  EXPECT_FALSE(take(Model->resumeFileAPI(Operation)));
  const auto Close = guest();
  ASSERT_GE(Close.Arguments.size(), 2u);
  EXPECT_EQ(read(read(Close.Arguments[1] + IRPStackPointerOffset), 1), 2u);
  EXPECT_EQ(read(HandleOut), 0x1122334455667788u);
  EXPECT_EQ(forward(Close.Arguments[1]), 0u);
  take(Model->finishGuestCall(Close.Token, 0));
  EXPECT_EQ(finish(Operation), StatusAccessDenied);
  EXPECT_EQ(read(HandleOut), 0x1122334455667788u);
  EXPECT_EQ(read(StatusOut), 0x8877665544332211u);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
  EXPECT_EQ(Result.Requests.size(), 3u);
  for (const auto &R : Result.Requests) {
    EXPECT_EQ(R.FileObject, File);
    EXPECT_EQ(R.FileSystemFileID, 1u);
    EXPECT_EQ(R.FileOpenOperation, Operation.ID);
  }
  ok(Model->snapshot());
}

TEST_F(KernelFileCreate, PendingOpenUsesFinalStatusAndKeepsCallerIdentity) {
  attach();
  ok(Model->setUserRequestContext(true, 0x2000));
  name("\\Device\\OpenVolume\\Data");
  const auto Operation = start();
  const auto Dispatch = guest();
  const auto IRP = Dispatch.Arguments[1];
  const auto File = read(IRP + IRPOriginalFileOffset);
  // Seed the model-owned status to distinguish deferred notification from a
  // worker publishing the eventual success value early.
  ok(Memory->writeInteger(File + FileFinalStatusOffset, 0xdeadbeef, 4));
  write(read(IRP + IRPStackPointerOffset) + StackControlOffset,
        StackPendingReturned, 1);
  take(Model->finishGuestCall(Dispatch.Token, StatusPending));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(profile::CallbackStackBase);
  EXPECT_EQ(forward(IRP), 0u);
  EXPECT_EQ(read(File + FileFinalStatusOffset, 4), 0xdeadbeefu);
  EXPECT_EQ(call("KeReadStateEvent", {File + FileEventOffset}), 0u);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  rejects(Model->resumeFileAPI(Operation), "original caller");
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(finish(Operation), 0u);
  EXPECT_EQ(read(File + FileFinalStatusOffset, 4), 0u);
  EXPECT_EQ(call("KeReadStateEvent", {File + FileEventOffset}), 1u);
  EXPECT_EQ(Result.Requests.back().DispatchStatus, StatusPending);
  EXPECT_EQ(Result.Requests.back().IOStatus, 0u);
  EXPECT_EQ(Result.Requests.back().RequestorProcessID, 0x2000u);
  EXPECT_NE(read(HandleOut), 0u);
}

TEST_F(KernelFileCreate, UnsupportedFilterRewritesStopBeforeProviderOpen) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  const auto Operation = start();
  const auto Dispatch = guest();
  ASSERT_GE(Dispatch.Arguments.size(), 2u);
  const auto IRP = Dispatch.Arguments[1];
  const auto Stack = read(IRP + IRPStackPointerOffset);
  const auto File = read(IRP + IRPOriginalFileOffset);
  const auto NameStorage = read(File + FileNameBufferOffset);
  std::array<uint8_t, StackSize> Bytes;
  ok(Memory->read(Stack, Bytes));
  ok(Memory->write(Stack - StackSize, Bytes));
  for (unsigned Shape = 0; Shape != 3; ++Shape) {
    SCOPED_TRACE(Shape);
    write(Stack - StackSize + 0x20, Shape == 0 ? 1 : 0, 4);
    if (Shape == 1)
      rejects(Model->validateGuestAccess(IRP + IRPSystemBufferOffset, 8, true),
              "read-only WDM");
    // The terminal provider also validates malformed backing independently of
    // the guest field gate; the system-buffer pointer itself is read-only.
    ok(Memory->writeInteger(IRP + IRPSystemBufferOffset,
                            Shape == 1 ? Scratch + 0x900 : 0, 8));
    write(File + FileNameBufferOffset,
          Shape == 2 ? Scratch + 0x900 : NameStorage);
    rejects(Model->call("IofCallDriver", {Volume, IRP}),
            Shape == 2 ? "replacement filename" : "extended attributes");
    EXPECT_FALSE(Result.Requests.back().Completed);
    EXPECT_FALSE(Result.Requests.back().FileSystemFileID);
  }
  write(File + FileNameBufferOffset, NameStorage);
  EXPECT_EQ(forward(IRP), 0u);
  take(Model->finishGuestCall(Dispatch.Token, 0));
  EXPECT_EQ(finish(Operation), 0u);
  EXPECT_EQ(Result.Requests.back().FileSystemFileID, 1u);
  ok(Model->snapshot());
}

TEST_F(KernelFileCreate, EntryRetainedHandleClosesInsideUnload) {
  const auto Handle = open();
  const auto File = *Result.Requests.front().FileObject;
  call("ObfReferenceObject", {File});
  write(Model->driverObject() + DriverUnloadOffset, Entry);
  ok(Model->finishEntry());
  take(Model->beginUnload());
  name("\\Device\\OpenVolume\\Data");
  rejects(Model->call("ZwOpenFile",
                      {HandleOut, 0x100081, Scratch, StatusOut, 7, 0x60}),
          "unload");
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  rejects(Model->finishUnload(), "live object references");
  call("ObfDereferenceObject", {File});
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(KernelFileCreate, AllCreateWrappersPublishAliasedOutputsInTargetOrder) {
  for (bool Io : {false, true}) {
    SCOPED_TRACE(Io);
    name("\\Device\\OpenVolume\\Data");
    const auto Before = Result.Requests.size();
    if (Io)
      call("IoCreateFile", {StatusOut + 8, 0x100081, Scratch, StatusOut, 0, 0,
                            7, 1, 0x60, 0, 0, 0, 0, 0x200});
    else
      call("ZwCreateFile", {StatusOut + 8, 0x100081, Scratch, StatusOut, 0, 0,
                            7, 1, 0x60, 0, 0});
    const auto Operation = Model->takeFileAPIContinuation();
    ASSERT_TRUE(Operation);
    EXPECT_EQ(finish(*Operation), 0u);
    EXPECT_EQ(read(StatusOut, 4), 0u);
    EXPECT_EQ(read(StatusOut + 8), 1u);
    EXPECT_EQ(Result.Requests.size(), Before + 1);
  }
}

TEST_F(KernelFileCreate, UpperFailureRequiresExplicitCancellation) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  start();
  const auto Create = guest();
  const auto IRP = Create.Arguments[1];
  EXPECT_EQ(forward(IRP, Entry + 0x20), 0u);
  const auto Completion = guest();
  write(IRP + IRPStatusOffset, StatusAccessDenied, 4);
  write(IRP + IRPInformationOffset, 0);
  rejects(Model->finishGuestCall(Completion.Token, 0), "IoCancelFileOpen");
  EXPECT_EQ(Result.Requests.size(), 1u);
  EXPECT_FALSE(Result.Requests.front().Completed);
}

TEST_F(KernelFileCreate,
       PendingRelativeOpenRetainsClosedRootUntilCallerRelease) {
  attach();
  name("\\Device\\OpenVolume\\Folder");
  const auto RootOperation = start(0x100081, 7, 0x21);
  const auto RootCreate = guest();
  ASSERT_GE(RootCreate.Arguments.size(), 2u);
  EXPECT_EQ(forward(RootCreate.Arguments[1]), 0u);
  take(Model->finishGuestCall(RootCreate.Token, 0));
  EXPECT_EQ(finish(RootOperation), 0u);
  const auto Root = read(HandleOut);
  const auto RootFile = *Result.Requests.front().FileObject;
  name("Child", Root);
  const auto Operation = start();
  const auto Create = guest();
  ASSERT_GE(Create.Arguments.size(), 2u);
  const auto IRP = Create.Arguments[1];
  const auto File = read(IRP + IRPOriginalFileOffset);
  write(read(IRP + IRPStackPointerOffset) + StackControlOffset,
        StackPendingReturned, 1);
  take(Model->finishGuestCall(Create.Token, StatusPending));
  const auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_EQ(call("ZwClose", {Root}), 0u);
  const auto Cleanup = guest();
  EXPECT_EQ(forward(Cleanup.Arguments[1]), 0u);
  take(Model->finishGuestCall(Cleanup.Token, 0));
  EXPECT_FALSE(Model->takeGuestCall());
  ok(Model->validateGuestAccess(RootFile, 1, false));
  EXPECT_EQ(forward(IRP), 0u);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  write(HandleOut, 0xdeadbeef);
  EXPECT_FALSE(take(Model->resumeFileAPI(Operation)));
  const auto Close = guest();
  ASSERT_GE(Close.Arguments.size(), 2u);
  EXPECT_EQ(read(Close.Arguments[1] + IRPOriginalFileOffset), RootFile);
  EXPECT_EQ(read(HandleOut), 0xdeadbeefu);
  EXPECT_EQ(forward(Close.Arguments[1]), 0u);
  take(Model->finishGuestCall(Close.Token, 0));
  EXPECT_EQ(finish(Operation), 0u);
  EXPECT_NE(read(HandleOut), 0xdeadbeefu);
  rejects(Model->validateGuestAccess(RootFile, 1, false), "freed");
  ok(Model->validateGuestAccess(File, 1, false));
  EXPECT_EQ(Result.Requests[1].FileSystemFileID, 3u);
}

TEST_F(KernelFileCreate, CaseAndLengthUseFileSuffixWithoutWideningDeviceNames) {
  name("\\Device\\OpenVolume\\data", 0, ObjectKernelHandle);
  EXPECT_EQ(finish(start()), StatusObjectNameNotFound);
  const auto Handle = open("\\device\\openvolume\\data");
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  name("\\Device\\OpenVolume\\" + std::string(600, 'x'));
  EXPECT_EQ(finish(start()), StatusObjectNameNotFound);
  name("\\Device\\OpenVolume");
  rejects(Model->call("ZwOpenFile",
                      {HandleOut, 0x100081, Scratch, StatusOut, 7, 0x60}),
          "volume opens");
  name("\\Device\\OpenVolume\\");
  EXPECT_EQ(finish(start(0x100081, 7, 0x21)), 0u);
  EXPECT_EQ(Result.Requests.back().FileSystemFileID, 0u);
  EXPECT_EQ(call("ZwClose", {read(HandleOut)}), 0u);
}

TEST_F(KernelFileCreate, UnsupportedShapesAndInvalidOutputsHaveNoOpenEffects) {
  name("\\Device\\OpenVolume\\Data");
  for (uint32_t Disposition : {0, 2, 3, 4, 5})
    rejects(
        Model->call("ZwCreateFile", {HandleOut, 0x100081, Scratch, StatusOut, 0,
                                     0, 7, Disposition, 0x60, 0, 0}),
        "existing-node");
  for (uint32_t Options : {0x10, 0x1000, 0x2000, 0x1000000})
    rejects(Model->call("ZwOpenFile",
                        {HandleOut, 0x100081, Scratch, StatusOut, 7, Options}),
            "FILE_OPEN");
  EXPECT_EQ(
      call("ZwOpenFile", {HandleOut, 0x100081, Scratch, StatusOut, 8, 0x60}),
      StatusInvalidParameter);
  EXPECT_EQ(call("ZwOpenFile", {HandleOut, 0x81, Scratch, StatusOut, 7, 0x60}),
            StatusInvalidParameter);
  rejects(Model->call("IoCreateFile", {HandleOut, 0x100081, Scratch, StatusOut,
                                       0, 0, 7, 1, 0x60, 0, 0, 1, 0, 0}),
          "special create");
  call("KeEnterGuardedRegion", {});
  rejects(Model->call("ZwOpenFile",
                      {HandleOut, 0x100081, Scratch, StatusOut, 7, 0x60}),
          "special kernel APCs");
  call("KeLeaveGuardedRegion", {});
  ok(Memory->map(Scratch + 0x20000, 0x1000, Read));
  rejects(Model->call("ZwOpenFile", {Scratch + 0x20000, 0x100081, Scratch,
                                     StatusOut, 7, 0x60}),
          "writable");
  EXPECT_TRUE(Result.Requests.empty());
  EXPECT_FALSE(Model->takeFileAPIContinuation());
  EXPECT_FALSE(Model->takeGuestCall());
  ok(Model->snapshot());
}

TEST_F(KernelFileCreate, WarningPublishesStatusAndInformationWithoutHandle) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  write(StatusOut, 0xaabbccdd11223344);
  write(StatusOut + 8, 0xdeadbeef);
  const auto Operation = start(0x100081, 7, 0x60, StatusOut + 8);
  const auto Dispatch = guest();
  const auto IRP = Dispatch.Arguments[1];
  const auto File = read(IRP + IRPOriginalFileOffset);
  write(IRP + IRPStatusOffset, 0x80000005, 4);
  write(IRP + IRPInformationOffset, 2);
  call("IofCompleteRequest", {IRP, 0});
  take(Model->finishGuestCall(Dispatch.Token, 0x80000005));
  EXPECT_EQ(finish(Operation), 0x80000005u);
  EXPECT_EQ(read(StatusOut), 0xaabbccdd80000005u);
  EXPECT_EQ(read(StatusOut + 8), 2u);
  EXPECT_EQ(Result.Requests.size(), 1u);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
}

TEST_F(KernelFileCreate, NamespaceLinksRetainDeviceSuffix) {
  name("\\Device\\OpenVolume");
  std::array<uint8_t, 16> Target;
  ok(Memory->read(Scratch + 0x80, Target));
  ok(Memory->write(Scratch + 0x180, Target));
  // Keep the target name bytes separate while preparing the link name.
  write(Scratch + 0x188, Scratch + 0x1000);
  std::array<uint8_t, 128> TargetBytes;
  ok(Memory->read(Scratch + 0x200, TargetBytes));
  ok(Memory->write(Scratch + 0x1000, TargetBytes));
  name("\\DosDevices\\OpenAlias");
  EXPECT_EQ(call("IoCreateSymbolicLink", {Scratch + 0x80, Scratch + 0x180}),
            0u);
  const auto Handle = open("\\??\\OpenAlias\\Data");
  EXPECT_EQ(Result.Requests.front().FileSystemFileID, 1u);
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
  name("\\DosDevices\\OpenAlias");
  EXPECT_EQ(call("IoDeleteSymbolicLink", {Scratch + 0x80}), 0u);
}

TEST_F(KernelFileCreate,
       FailedDriverCreatePreservesScenarioZeroAndCloseIntent) {
  call("IoCreateDevice",
       {Model->driverObject(), 0, 0, 0x22, 0, 0, Scratch + 0x180});
  for (unsigned Major : {0, 2, 18})
    write(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
  auto Scenario = [&](DriverRequestKind Kind) {
    DriverRequest Request;
    Request.Kind = Kind;
    Request.File = 0;
    auto Call = take(Model->beginRequest(Request));
    EXPECT_NE(Call.IRP, 0u);
    if (!Call.IRP)
      return uint64_t{0};
    const auto File = read(Call.IRP + IRPOriginalFileOffset);
    write(Call.IRP + IRPStatusOffset, 0, 4);
    write(Call.IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {Call.IRP, 0});
    ok(Model->recordDispatchReturn(Call.IRP, 0));
    ok(Model->finalizeRequest(Call.IRP));
    return File;
  };
  const auto File = Scenario(DriverRequestKind::Create);
  name("\\Device\\OpenVolume\\Missing");
  EXPECT_EQ(finish(start()), StatusObjectNameNotFound);
  EXPECT_EQ(Scenario(DriverRequestKind::Cleanup), File);
  EXPECT_EQ(Result.Requests.size(), 3u);
  EXPECT_FALSE(Model->takeGuestCall());
  ok(Model->validateGuestAccess(File, 1, false));
  EXPECT_EQ(Scenario(DriverRequestKind::Close), File);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
}
class KernelDevicePointer : public KernelFileCreate {
protected:
  KernelModel::FileAPIContinuation pointer(uint64_t FileOutput = HandleOut,
                                           uint64_t DeviceOutput = StatusOut,
                                           uint64_t Access = 0x100081) {
    EXPECT_EQ(call("IoGetDeviceObjectPointer",
                   {Scratch + 0x80, Access, FileOutput, DeviceOutput}),
              0u);
    auto Pending = Model->takeFileAPIContinuation();
    EXPECT_TRUE(Pending);
    return Pending.value_or(KernelModel::FileAPIContinuation{});
  }
};

TEST_F(KernelDevicePointer, RetainsOnlyTheFileUntilItsFinalReferenceRelease) {
  name("\\device\\openvolume\\data");
  call("KeEnterCriticalRegion", {});
  const auto Operation = pointer();
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
  EXPECT_EQ(finish(Operation), 0u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
  call("KeLeaveCriticalRegion", {});
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  const auto File = read(HandleOut);
  EXPECT_EQ(read(StatusOut), Volume);
  EXPECT_EQ(read(File + FileFlagsOffset, 4) & FileSynchronousIO, 0u);
  EXPECT_TRUE(read(File + FileFlagsOffset, 4) & FileCleanupComplete);
  EXPECT_EQ(read(Volume + DeviceReferenceCount, 4), 1u);
  ASSERT_EQ(Result.Requests.size(), 2u);
  EXPECT_EQ(Result.Requests[0].Kind, DriverRequestKind::Create);
  EXPECT_EQ(Result.Requests[1].Kind, DriverRequestKind::Cleanup);
  EXPECT_EQ(call("IoGetRelatedDeviceObject", {File}), Volume);
  // CLEANUP has released the wrapper's sharing grant, even while its pointer
  // reference retains the file body and the device.
  const auto Other = open("\\Device\\OpenVolume\\Data", 0x100002, 0);
  EXPECT_EQ(call("ZwClose", {Other}), 0u);
  call("ObfDereferenceObject", {File});
  EXPECT_EQ(Result.Requests.back().Kind, DriverRequestKind::Close);
  EXPECT_EQ(read(Volume + DeviceReferenceCount, 4), 0u);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
  ok(Model->snapshot());
}

TEST_F(KernelDevicePointer, AliasedOutputsKeepThePrivateReferencedObject) {
  for (unsigned Offset : {0, 4, 8}) {
    SCOPED_TRACE(Offset);
    name("\\Device\\OpenVolume\\Data");
    ok(Memory->write(HandleOut, std::vector<uint8_t>(24, 0xa5)));
    EXPECT_EQ(finish(pointer(HandleOut, HandleOut + Offset)), 0u);
    const auto File = *Result.Requests[Result.Requests.size() - 2].FileObject;
    std::array<uint8_t, 24> Expected;
    Expected.fill(0xa5);
    for (unsigned I = 0; I != 8; ++I)
      Expected[I] = uint8_t(File >> (I * 8));
    for (unsigned I = 0; I != 8; ++I)
      Expected[Offset + I] = uint8_t(Volume >> (I * 8));
    std::array<uint8_t, 24> Observed;
    ok(Memory->read(HandleOut, Observed));
    EXPECT_EQ(Observed, Expected);
    call("ObfDereferenceObject", {File});
    EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  }
  ok(Model->snapshot());
}

TEST_F(KernelDevicePointer, FailedOpensLeaveOutputsAndApcDepthUntouched) {
  name("\\Device\\MissingVolume\\Data");
  // No output access is required on the failed namespace path.
  EXPECT_EQ(call("IoGetDeviceObjectPointer", {Scratch + 0x80, 0, 0, 0}),
            StatusObjectPathNotFound);
  EXPECT_FALSE(Model->takeFileAPIContinuation());
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  for (auto [Path, Status] : {std::pair{"\\Missing", StatusObjectNameNotFound},
                              {"\\Folder", StatusFileIsADirectory}}) {
    name(std::string("\\Device\\OpenVolume") + Path);
    write(HandleOut, 0x1122334455667788);
    write(StatusOut, 0x8877665544332211);
    EXPECT_EQ(finish(pointer()), Status);
    EXPECT_EQ(read(HandleOut), 0x1122334455667788u);
    EXPECT_EQ(read(StatusOut), 0x8877665544332211u);
    EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
    rejects(Model->validateGuestAccess(*Result.Requests.back().FileObject, 1,
                                       false),
            "freed");
  }
  ok(Model->snapshot());
}

TEST_F(KernelDevicePointer, PendingCreateAndCleanupRestoreTheOriginalCaller) {
  attach();
  ok(Model->setUserRequestContext(true, 0x2000));
  name("\\Device\\OpenVolume\\Data");
  write(HandleOut, 0xdeadbeef);
  const auto Operation = pointer(HandleOut, StatusOut, 0x100100081);
  const auto Create = guest();
  const auto IRP = Create.Arguments[1];
  const auto File = read(IRP + IRPOriginalFileOffset);
  const auto Stack = read(IRP + IRPStackPointerOffset);
  EXPECT_EQ(read(Stack + StackInputLengthOffset, 4), 0x1000040u);
  EXPECT_EQ(read(Stack + StackCreateShareOffset, 2), 3u);
  EXPECT_EQ(read(read(Stack + StackParametersOffset) + 0x10, 4), 0x100081u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
  EXPECT_EQ(call("KeAreAllApcsDisabled", {}), 0u);
  write(Stack + StackControlOffset, StackPendingReturned, 1);
  take(Model->finishGuestCall(Create.Token, StatusPending));
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(profile::CallbackStackBase);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  EXPECT_EQ(forward(IRP), 0u);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  rejects(Model->resumeFileAPI(Operation), "original caller");
  EXPECT_EQ(read(HandleOut), 0xdeadbeefu);
  Model->enterExecution(profile::StackBase);
  EXPECT_FALSE(take(Model->resumeFileAPI(Operation)));
  // Publication precedes private-handle CLEANUP and survives its wait.
  EXPECT_EQ(read(HandleOut), File);
  EXPECT_EQ(read(StatusOut), Filter);
  const auto Cleanup = guest();
  const auto CleanupIRP = Cleanup.Arguments[1];
  EXPECT_EQ(read(read(CleanupIRP + IRPStackPointerOffset), 1), 18u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
  write(read(CleanupIRP + IRPStackPointerOffset) + StackControlOffset,
        StackPendingReturned, 1);
  take(Model->finishGuestCall(Cleanup.Token, StatusPending));
  Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  EXPECT_FALSE(take(Model->pollWait(*Wait)));
  Model->enterExecution(profile::CallbackStackBase);
  EXPECT_EQ(forward(CleanupIRP), 0u);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>{0});
  rejects(Model->resumeFileAPI(Operation), "original caller");
  Model->enterExecution(profile::StackBase);
  rejects(Model->resumeFileAPI(Operation), "private-handle close");
  EXPECT_EQ(take(Model->resumeFileHandleClose(Wait->Object)),
            std::optional<uint64_t>{0});
  EXPECT_EQ(finish(Operation), 0u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  EXPECT_EQ(Result.Requests.front().RequestorProcessID, 0x2000u);
  // The private kernel handle belongs to the system table; final CLOSE still
  // records the releasing caller, independently of that handle table.
  EXPECT_EQ(Result.Requests.back().RequestorProcessID, SystemProcessID);
  call("ObfDereferenceObject", {File});
  const auto Close = guest();
  EXPECT_EQ(forward(Close.Arguments[1]), 0u);
  take(Model->finishGuestCall(Close.Token, 0));
  EXPECT_EQ(Result.Requests.back().RequestorProcessID, 0x2000u);
  rejects(Model->validateGuestAccess(File, 1, false), "freed");
  ok(Model->snapshot());
}

TEST_F(KernelDevicePointer, WarningIsAnOpenFailureWithoutPointerPublication) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  write(HandleOut, 0x1122334455667788);
  write(StatusOut, 0x8877665544332211);
  const auto Operation = pointer();
  const auto Create = guest();
  const auto IRP = Create.Arguments[1];
  write(IRP + IRPStatusOffset, 0x80000005, 4);
  write(IRP + IRPInformationOffset, 2);
  call("IofCompleteRequest", {IRP, 0});
  take(Model->finishGuestCall(Create.Token, 0x80000005));
  EXPECT_EQ(finish(Operation), 0x80000005u);
  EXPECT_EQ(read(HandleOut), 0x1122334455667788u);
  EXPECT_EQ(read(StatusOut), 0x8877665544332211u);
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  EXPECT_EQ(Result.Requests.size(), 1u);
}

TEST_F(KernelDevicePointer,
       SuccessfulReferenceReplacesInformationalOpenStatus) {
  attach();
  name("\\Device\\OpenVolume\\Data");
  const auto Operation = pointer();
  const auto Create = guest();
  const auto IRP = Create.Arguments[1];
  EXPECT_EQ(forward(IRP, Entry + 0x20), 0u);
  const auto Completion = guest();
  write(IRP + IRPStatusOffset, 0x40000001, 4);
  take(Model->finishGuestCall(Completion.Token, 0));
  take(Model->finishGuestCall(Create.Token, 0x40000001));
  EXPECT_FALSE(take(Model->resumeFileAPI(Operation)));
  const auto Cleanup = guest();
  EXPECT_EQ(forward(Cleanup.Arguments[1]), 0u);
  take(Model->finishGuestCall(Cleanup.Token, 0));
  EXPECT_EQ(finish(Operation), 0u);
  EXPECT_EQ(Result.Requests.front().IOStatus, 0x40000001u);
  EXPECT_EQ(read(StatusOut), Filter);
}

TEST_F(KernelDevicePointer, DeviceOutputFaultPreservesTheEarlierFileStore) {
  name("\\Device\\OpenVolume\\Data");
  const auto ReadOnly = Scratch + 0x20000;
  ok(Memory->map(ReadOnly, 0x1000, Read));
  write(HandleOut, 0xdeadbeef);
  const auto Operation = pointer(HandleOut, ReadOnly);
  rejects(Model->resumeFileAPI(Operation), "writable");
  EXPECT_EQ(read(HandleOut), Result.Requests.front().FileObject);
  EXPECT_NE(read(HandleOut), 0xdeadbeefu);
  EXPECT_EQ(read(ReadOnly), 0u);
  EXPECT_EQ(Result.Requests.size(), 1u);
  // This terminal model error does not claim a completed wrapper or cleanup.
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 1u);
}

TEST_F(KernelDevicePointer, AdmissionFailuresBalanceTheWrappersCriticalRegion) {
  name("\\Device\\OpenVolume\\Data");
  call("KeEnterGuardedRegion", {});
  rejects(Model->call("IoGetDeviceObjectPointer",
                      {Scratch + 0x80, 0, HandleOut, StatusOut}),
          "special kernel APCs");
  call("KeLeaveGuardedRegion", {});
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  call("KfRaiseIrql", {1});
  rejects(Model->call("IoGetDeviceObjectPointer",
                      {Scratch + 0x80, 0, HandleOut, StatusOut}),
          "PASSIVE_LEVEL");
  call("KeLowerIrql", {0});
  name("\\Device\\OpenVolume");
  rejects(Model->call("IoGetDeviceObjectPointer",
                      {Scratch + 0x80, 0, HandleOut, StatusOut}),
          "volume opens");
  EXPECT_EQ(call("KeAreApcsDisabled", {}), 0u);
  EXPECT_TRUE(Result.Requests.empty());
  EXPECT_FALSE(Model->takeFileAPIContinuation());
}

TEST_F(KernelDevicePointer, EntryReferenceMustBeReleasedBeforeUnloadCompletes) {
  name("\\Device\\OpenVolume\\Data");
  EXPECT_EQ(finish(pointer()), 0u);
  const auto File = read(HandleOut);
  write(Model->driverObject() + DriverUnloadOffset, Entry);
  ok(Model->finishEntry());
  take(Model->beginUnload());
  rejects(Model->finishUnload(), "live object references");
  call("ObfDereferenceObject", {File});
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}
} // namespace
} // namespace neverd::emulation
