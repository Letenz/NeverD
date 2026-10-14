//===- KernelVersionQueryTests.cpp - Declared query memory boundaries
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent version query memory and object boundary controls.
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
constexpr uint64_t Limit = 16 * 1024 * 1024;
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
  void initialize(std::optional<DriverVersionQuery> Query = DriverVersionQuery{
                      1, 12345, "independent query", true}) {
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.VersionQuery = std::move(Query);
    checked(Model.initialize(Image, Options));
  }
  std::vector<uint8_t> bytes(uint64_t Address = Scratch, size_t Count = 320) {
    std::vector<uint8_t> B(Count);
    checked(Memory->snapshotBacking(Address, B));
    return B;
  }
  void prepare(uint32_t Size, uint64_t Address = Scratch, size_t Count = 320) {
    std::vector<uint8_t> B(Count, 0xa5);
    llvm::support::endian::write32le(B.data(), Size);
    if (Size == 276 && Count >= 276)
      std::fill(B.begin() + 20, B.begin() + 276, 0);
    checked(Memory->write(Address, B));
  }
  uint64_t beginUserRequest() {
    using namespace windows;
    EXPECT_EQ(checked(Model.call("IoCreateDevice",
                                 {Model.driverObject(), 0, 0, UnknownDeviceType,
                                  0, 0, Scratch})),
              0u);
    for (unsigned Major : {0u, 14u}) {
      const uint64_t Slot =
          Model.driverObject() + DriverDispatchOffset + Major * 8;
      checked(Model.validateGuestAccess(Slot, 8, true));
      checked(Memory->writeInteger(Slot, Base, 8));
    }
    checked(Model.finishEntry());
    DriverRequest Create;
    Create.Kind = DriverRequestKind::Create;
    Create.File = 1;
    const auto Open = checked(Model.beginRequest(Create));
    checked(Memory->writeInteger(Open.IRP + IRPStatusOffset, 0, 4));
    checked(Memory->writeInteger(Open.IRP + IRPInformationOffset, 0, 8));
    checked(Model.call("IofCompleteRequest", {Open.IRP, 0}));
    checked(Model.recordDispatchReturn(Open.IRP, 0));
    checked(Model.finalizeRequest(Open.IRP));
    DriverRequest IO;
    IO.Kind = DriverRequestKind::DeviceControl;
    IO.File = 1;
    IO.ControlCode = 0x222000 | MethodNeither;
    IO.OutputSize = Page;
    const auto Request = checked(Model.beginRequest(IO));
    Model.enterExecution(profile::StackBase);
    checked(Model.setUserRequestContext(
        true, DriverRequest::DefaultRequestorProcessID));
    return checked(Memory->readInteger(Request.IRP + IRPUserBufferOffset, 8));
  }
};

TEST(KernelVersionQuery, DeclaredBuildAndOnlyBaseFieldsChange) {
  for (uint32_t Build : {0u, 12345u, 65535u})
    for (uint32_t Size : {0u, 276u}) {
      Session S;
      S.initialize(
          DriverVersionQuery{1, Build, "independent range oracle", true});
      S.prepare(Size);
      auto Expected = S.bytes();
      for (auto [Offset, Value] :
           {std::pair{4u, 10u}, {8u, 0u}, {12u, Build}, {16u, 2u}})
        llvm::support::endian::write32le(Expected.data() + Offset, Value);
      EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Scratch})), 0u);
      EXPECT_EQ(S.bytes(), Expected);
      EXPECT_EQ(S.Model.currentIRQL(), 0u);
      EXPECT_FALSE(S.Model.takeGuestCall());
    }
}

TEST(KernelVersionQuery, ZeroShapeNeverReadsPastTwentyBytes) {
  Session S;
  S.initialize();
  const uint64_t Tail = Scratch + Page - 20;
  S.prepare(0, Tail, 20);
  EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Tail})), 0u);
  EXPECT_EQ(checked(S.Memory->readInteger(Tail + 12, 4)), 12345u);
  EXPECT_FALSE(checked(S.Memory->canAccess(Tail + 20, 1, Read)));
}

TEST(KernelVersionQuery, ShapesAndCSDRejectWithoutPartialOutput) {
  Session S;
  S.initialize(DriverVersionQuery{1, 0, "zero not enabled", false});
  for (uint32_t Size : {0u, 1u, 20u, 275u, 277u, 284u, 292u, UINT32_MAX}) {
    S.prepare(Size);
    const auto Before = S.bytes();
    denied(S.Model.call("RtlGetVersion", {Scratch}), "size is unsupported");
    EXPECT_EQ(S.bytes(), Before);
  }
  for (unsigned Offset : {20u, 21u, 274u, 275u}) {
    S.prepare(276);
    checked(S.Memory->writeInteger(Scratch + Offset, 1, 1));
    const auto Before = S.bytes();
    denied(S.Model.call("RtlGetVersion", {Scratch}), "zeroed CSD");
    EXPECT_EQ(S.bytes(), Before);
  }
}

TEST(KernelVersionQuery, MissingPermissionsAndExtentsPreserveAllBytes) {
  Session S;
  S.initialize();
  S.prepare(276);
  const auto Before = S.bytes();
  for (uint64_t Address :
       {uint64_t(0), Scratch + Page - 3, UINT64_MAX - 2, UINT64_MAX - 19}) {
    auto Result = S.Model.call("RtlGetVersion", {Address});
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(S.bytes(), Before);
  }
  const uint64_t Tail = Scratch + Page - 20;
  for (uint32_t Size : {0u, 276u}) {
    S.prepare(Size, Tail, 20);
    if (!Size) {
      EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Tail})), 0u);
      // Same values still require write permission.
    }
    const auto TailBefore = S.bytes(Tail, 20);
    checked(S.Memory->protect(Scratch, Page, Read));
    auto Result = S.Model.call("RtlGetVersion", {Tail});
    ASSERT_FALSE(bool(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(S.bytes(Tail, 20), TailBefore);
    checked(S.Memory->protect(Scratch, Page, Read | Write));
  }
  S.prepare(0, Tail, 20);
  const auto TailBefore = S.bytes(Tail, 20);
  checked(S.Memory->protect(Scratch, Page, Write));
  denied(S.Model.call("RtlGetVersion", {Tail}), "accessible ordinary RAM");
  EXPECT_EQ(S.bytes(Tail, 20), TailBefore);
}

TEST(KernelVersionQuery, SharedOwnedRecordGuardsApplyBeforeOutput) {
  Session S;
  S.initialize();
  const uint64_t Record = Base + Page;
  EXPECT_EQ(checked(S.Model.call("KeRegisterBugCheckReasonCallback",
                                 {Record, Base, 2, Scratch})),
            1u);
  // Native zero-size shape starts in the high zero half of Component;
  // this complete input extent includes opaque checksum bytes.
  const auto Before = S.bytes(Record, 48);
  denied(S.Model.call("RtlGetVersion", {Record + 28}), "opaque");
  EXPECT_EQ(S.bytes(Record, 48), Before);
}

TEST(KernelVersionQuery, CrossPageWriteAndBorrowedFieldsKeepAllOriginalBytes) {
  Session S;
  S.initialize();
  checked(S.Memory->map(Scratch + Page, Page, Read | Write));
  const uint64_t Cross = Scratch + Page - 12;
  S.prepare(0, Cross, 40);
  const auto Before = S.bytes(Cross, 40);
  checked(S.Memory->protect(Scratch + Page, Page, Read));
  denied(S.Model.call("RtlGetVersion", {Cross}), "accessible ordinary RAM");
  EXPECT_EQ(S.bytes(Cross, 40), Before);

  Session Borrowed;
  // Metadata-only driver identity whose low DllBase DWORD is zero. The
  // complete query read lies in known loader fields, but writes are forbidden.
  Borrowed.Image.Base = 0x200000000;
  Borrowed.Image.Entry = Borrowed.Image.Base;
  Borrowed.Image.Regions.clear();
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.LoaderView = DriverLoaderView{1, 0, "borrowed fields"};
  Options.VersionQuery = DriverVersionQuery{1, 42, "borrowed fields", true};
  checked(Borrowed.Model.initialize(Borrowed.Image, Options));
  const uint64_t Loader = checked(
      Borrowed.Memory->readInteger(Borrowed.Model.driverObject() + 40, 8));
  const auto Original = Borrowed.bytes(Loader, 160);
  denied(Borrowed.Model.call("RtlGetVersion", {Loader + 48}), "read-only");
  EXPECT_EQ(Borrowed.bytes(Loader, 160), Original);
}

TEST(KernelVersionQuery, UserObjectAndItsSystemAliasCannotBecomeOutput) {
  using namespace windows;
  Session S;
  S.initialize();
  const uint64_t User = S.beginUserRequest();
  const uint64_t MDL =
      checked(S.Model.call("IoAllocateMdl", {User, Page, 0, 0, 0}));
  checked(S.Model.call("MmProbeAndLockPages", {MDL, UserMode, IoModifyAccess}));
  const uint64_t Alias = checked(
      S.Model.call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority}));
  ASSERT_GE(Alias, profile::UserProbeLimit);
  checked(S.Model.call("KeInitializeEvent",
                       {User, dispatcher::NotificationObject, 1}));
  const auto Before = S.bytes(User);
  denied(S.Model.call("RtlGetVersion", {User}), "kernel buffer");
  EXPECT_EQ(S.bytes(User), Before);
  denied(S.Model.call("RtlGetVersion", {Alias}), "alias");
  EXPECT_EQ(S.bytes(Alias), Before);
  EXPECT_EQ(checked(S.Model.call("KeReadStateEvent", {User})), 1u);
}

TEST(KernelVersionQuery, UserAliasObjectAlsoProtectsOriginalPoolBytes) {
  using namespace windows;
  Session S;
  S.initialize();
  S.beginUserRequest();
  const uint64_t Pool =
      checked(S.Model.call("ExAllocatePoolWithTag", {0, Page, 0x56517279}));
  const uint64_t MDL =
      checked(S.Model.call("IoAllocateMdl", {Pool, Page, 0, 0, 0}));
  checked(S.Model.call("MmBuildMdlForNonPagedPool", {MDL}));
  const uint64_t Alias = checked(
      S.Model.call("MmMapLockedPagesSpecifyCache",
                   {MDL, UserMode, MmCached, 0, 0, NormalPagePriority}));
  checked(S.Model.call("KeInitializeEvent",
                       {Alias, dispatcher::NotificationObject, 1}));
  const auto Before = S.bytes(Pool);
  denied(S.Model.call("RtlGetVersion", {Pool}), "alias");
  EXPECT_EQ(S.bytes(Pool), Before);
  EXPECT_EQ(checked(S.Model.call("KeReadStateEvent", {Alias})), 1u);
}

TEST(KernelVersionQuery, AliasedBytesRefuseUntilTheOtherMappingIsRetired) {
  for (uint64_t Other : {Scratch - Page, Scratch + Page, UINT64_MAX - Page + 1})
    for (unsigned Permissions : {0u, unsigned(Read), unsigned(Read | Write)}) {
      Session S;
      S.initialize();
      S.prepare(0);
      const auto Before = S.bytes();
      // Adjacent equal-permission mappings coalesce in the layout inventory.
      // No-access aliases still retain the same physical bytes.
      checked(S.Memory->mapAlias(Other, Scratch, Page, Permissions));
      denied(S.Model.call("RtlGetVersion", {Scratch}), "alias");
      EXPECT_EQ(S.bytes(), Before);
      checked(S.Memory->unmapAlias(Other, Page));
      EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Scratch})), 0u);
      EXPECT_EQ(checked(S.Memory->readInteger(Scratch + 12, 4)), 12345u);
    }
}

TEST(KernelVersionQuery, UniqueCrossPageBufferAllowsUnrelatedAliases) {
  Session S;
  S.initialize();
  checked(S.Memory->map(Scratch + Page, Page, Read | Write));
  checked(
      S.Memory->mapAlias(Scratch + 2 * Page, Base + Page, Page, Read | Write));
  const uint64_t Cross = Scratch + Page - 12;
  S.prepare(276, Cross);
  auto Expected = S.bytes(Cross);
  for (auto [Offset, Value] :
       {std::pair{4u, 10u}, {8u, 0u}, {12u, 12345u}, {16u, 2u}})
    llvm::support::endian::write32le(Expected.data() + Offset, Value);
  EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Cross})), 0u);
  EXPECT_EQ(S.bytes(Cross), Expected);
}

TEST(KernelVersionQuery, CSDOnlyAliasIsCheckedBeforeBaseOutput) {
  Session S;
  S.initialize();
  checked(S.Memory->map(Scratch + Page, Page, Read | Write));
  const uint64_t Cross = Scratch + Page - 20;
  S.prepare(276, Cross);
  checked(S.Memory->mapAlias(Scratch + 2 * Page, Scratch + Page, Page, Read));
  const auto Before = S.bytes(Cross);
  denied(S.Model.call("RtlGetVersion", {Cross}), "alias");
  EXPECT_EQ(S.bytes(Cross), Before);
  // The separately enabled zero-size shape never accesses the aliased CSD.
  S.prepare(0, Cross);
  EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Cross})), 0u);
}

TEST(KernelVersionQuery, IRQLAndUndeclaredQueriesCannotPublishOutput) {
  for (bool Declared : {false, true}) {
    Session S;
    S.initialize(Declared ? std::optional<DriverVersionQuery>(
                                {1, 0, "IRQL control", true})
                          : std::nullopt);
    S.prepare(0);
    const auto Before = S.bytes();
    if (!Declared)
      denied(S.Model.call("RtlGetVersion", {Scratch}), "not declared");
    else {
      S.Model.enterExecution(profile::StackBase);
      EXPECT_EQ(checked(S.Model.call("KfRaiseIrql", {1})), 0u);
      denied(S.Model.call("RtlGetVersion", {Scratch}), "PASSIVE_LEVEL");
      checked(S.Model.call("KeLowerIrql", {0}));
      checked(S.Model.validateExecutionReturn(profile::StackBase, 0));
    }
    EXPECT_EQ(S.bytes(), Before);
  }
}

TEST(KernelVersionQuery, DeclarationIsCopiedAndBaselineCannotEraseDependency) {
  Session S;
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.VersionQuery = DriverVersionQuery{1, 42, "original", true};
  checked(S.Model.initialize(S.Image, Options));
  Options.VersionQuery->BuildNumber = 99;
  Options.VersionQuery->AllowZeroSize = false;
  checked(S.Model.captureUnpackBaseline());
  const auto Before = checked(S.Model.unpackDependencies());
  EXPECT_NE(std::find(Before.begin(), Before.end(),
                      "unreconstructed kernel or environment effects"),
            Before.end());
  S.prepare(0);
  EXPECT_EQ(checked(S.Model.call("RtlGetVersion", {Scratch})), 0u);
  EXPECT_EQ(checked(S.Memory->readInteger(Scratch + 12, 4)), 42u);
  EXPECT_EQ(checked(S.Model.unpackDependencies()), Before);
}

TEST(KernelVersionQuery, InvalidDeclarationsFailBeforeKernelAllocation) {
  for (auto Query : {DriverVersionQuery{0, 0, "bad", false},
                     DriverVersionQuery{2, 0, "bad", false},
                     DriverVersionQuery{1, std::nullopt, "missing", false},
                     DriverVersionQuery{1, 65536, "wide", false},
                     DriverVersionQuery{1, 0, "", false},
                     DriverVersionQuery{1, 0, std::string(257, 'x'), false},
                     DriverVersionQuery{1, 0, "bad\nlabel", false}}) {
    Session S;
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.VersionQuery = Query;
    denied(S.Model.initialize(S.Image, Options), "driver version_query:");
    EXPECT_EQ(S.Model.driverObject(), 0u);
    EXPECT_TRUE(S.Model.unpackAllocations().empty());
    S.initialize();
    EXPECT_NE(S.Model.driverObject(), 0u);
  }
  Session S;
  DriverOptions Legacy;
  Legacy.VersionQuery = DriverVersionQuery{1, 0, "legacy", false};
  denied(S.Model.initialize(S.Image, Legacy), "checked-x64");
  EXPECT_TRUE(S.Model.unpackAllocations().empty());
}
} // namespace
} // namespace neverd::emulation
