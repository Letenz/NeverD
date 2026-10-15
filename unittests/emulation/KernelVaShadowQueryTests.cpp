//===- KernelVaShadowQueryTests.cpp - Mitigation query memory guards ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent mitigation query memory and object boundary controls.
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
  void initialize(std::optional<DriverKernelVaShadowQuery> Query =
                      DriverKernelVaShadowQuery{1, 0x1234,
                                                "independent query"}) {
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.KernelVaShadowQuery = std::move(Query);
    checked(Model.initialize(Image, Options));
  }
  std::vector<uint8_t> bytes(uint64_t Address = Scratch, size_t Count = 320) {
    std::vector<uint8_t> B(Count);
    checked(Memory->snapshotBacking(Address, B));
    return B;
  }
  void prepare(uint8_t Fill = 0xa5) {
    checked(Memory->write(Scratch, std::vector<uint8_t>(320, Fill)));
  }
  llvm::Expected<uint64_t>
  query(uint64_t Out = Scratch, uint64_t Length = 4,
        uint64_t Returned = Scratch + 64,
        llvm::StringRef API = "ZwQuerySystemInformation",
        uint64_t Class = 196) {
    return Model.call(API.str(), {Class, Out, Length, Returned});
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

TEST(KernelVaShadowQuery, DeclaredFlagsAndExactFootprintsOnBothEntrypoints) {
  std::vector<uint32_t> Flags{0, 0x1234, 0x3fff};
  for (unsigned Bit = 0; Bit < 14; ++Bit)
    Flags.push_back(1u << Bit);
  for (uint32_t Flag : Flags) {
    Session S;
    S.initialize(DriverKernelVaShadowQuery{1, Flag, "independent bits"});
    S.Model.enterExecution(profile::StackBase);
    for (auto API : {"NtQuerySystemInformation", "ZwQuerySystemInformation"})
      for (uint64_t Length : {4u, 5u, 8u, UINT32_MAX})
        for (uint64_t Returned : {uint64_t(0), Scratch + 64}) {
          S.prepare();
          auto Expected = S.bytes();
          llvm::support::endian::write32le(Expected.data(), Flag);
          if (Returned)
            llvm::support::endian::write32le(Expected.data() + 64, 4);
          EXPECT_EQ(checked(S.query(Scratch, Length, Returned, API)), 0u);
          EXPECT_EQ(S.bytes(), Expected);
          EXPECT_EQ(checked(S.Model.call("ExGetPreviousMode", {})), 0u);
          EXPECT_EQ(S.Model.currentIRQL(), 0u);
          EXPECT_FALSE(S.Model.takeGuestCall());
        }
  }
}

TEST(KernelVaShadowQuery, ShortResultsOnlyWriteOptionalLength) {
  Session S;
  S.initialize();
  S.Model.enterExecution(profile::StackBase);
  for (auto API : {"NtQuerySystemInformation", "ZwQuerySystemInformation"})
    for (uint64_t Length : {0u, 1u, 2u, 3u})
      for (uint64_t Out : {uint64_t(0), Scratch, UINT64_MAX})
        for (uint64_t Returned : {uint64_t(0), Scratch + 64}) {
          S.prepare();
          auto Expected = S.bytes();
          if (Returned)
            llvm::support::endian::write32le(Expected.data() + 64, 4);
          EXPECT_EQ(checked(S.query(Out, Length, Returned, API)), 0xc0000004u);
          EXPECT_EQ(S.bytes(), Expected);
        }
}

TEST(KernelVaShadowQuery, ULONGArgumentsTruncateAndUnmappedTailIsUntouched) {
  Session S;
  S.initialize();
  const uint64_t Tail = Scratch + Page - 4;
  for (uint64_t Length : {0x100000004ULL, 0x100000003ULL, 0x1ffffffffULL}) {
    checked(S.Memory->writeInteger(Tail, 0xdeadbeef, 4));
    EXPECT_EQ(checked(S.query(Tail, Length, 0, "ZwQuerySystemInformation",
                              0x1000000c4ULL)),
              uint32_t(Length) < 4 ? 0xc0000004u : 0u);
    EXPECT_EQ(checked(S.Memory->readInteger(Tail, 4)),
              uint32_t(Length) < 4 ? 0xdeadbeefu : 0x1234u);
    EXPECT_FALSE(checked(S.Memory->canAccess(Tail + 4, 1, Read)));
  }
  const auto Before = S.bytes();
  denied(S.query(Scratch, 4, Scratch + 64, "ZwQuerySystemInformation", 197),
         "unsupported information class");
  EXPECT_EQ(S.bytes(), Before);
}

TEST(KernelVaShadowQuery, MissingDeclarationIRQLAndNtContextPreserveBytes) {
  Session Missing;
  Missing.initialize(std::nullopt);
  Missing.prepare();
  const auto Original = Missing.bytes();
  denied(Missing.query(), "not declared");
  EXPECT_EQ(Missing.bytes(), Original);
  Session S;
  S.initialize();
  S.prepare();
  const auto Before = S.bytes();
  denied(S.query(Scratch, 4, Scratch + 64, "NtQuerySystemInformation"),
         "unavailable");
  EXPECT_EQ(S.bytes(), Before);
  S.Model.enterExecution(profile::StackBase);
  checked(S.Model.setUserRequestContext(true));
  denied(S.query(Scratch, 4, Scratch + 64, "NtQuerySystemInformation"),
         "probing");
  EXPECT_EQ(S.bytes(), Before);
  EXPECT_EQ(checked(S.query()), 0u);
  EXPECT_EQ(checked(S.Model.call("ExGetPreviousMode", {})), 1u);
  checked(S.Model.setUserRequestContext(false));
  checked(S.Model.call("KfRaiseIrql", {1}));
  const auto BeforeIRQL = S.bytes();
  for (auto API : {"NtQuerySystemInformation", "ZwQuerySystemInformation"})
    denied(S.query(Scratch, 4, Scratch + 64, API), "PASSIVE_LEVEL");
  EXPECT_EQ(S.bytes(), BeforeIRQL);
  checked(S.Model.call("KeLowerIrql", {0}));
  EXPECT_EQ(
      checked(S.query(Scratch, 4, Scratch + 64, "NtQuerySystemInformation")),
      0u);
}

TEST(KernelVaShadowQuery, WritableLengthNeedsNoReadPermission) {
  Session S;
  S.initialize();
  const uint64_t Returned = Scratch + 2 * Page;
  checked(S.Memory->map(Returned, Page, Write));
  for (uint64_t Length : {0u, 3u, 4u, 8u}) {
    S.prepare();
    auto Expected = S.bytes();
    if (Length >= 4)
      llvm::support::endian::write32le(Expected.data(), 0x1234);
    EXPECT_EQ(checked(S.query(Scratch, Length, Returned)),
              Length < 4 ? 0xc0000004u : 0u);
    EXPECT_EQ(S.bytes(), Expected);
    EXPECT_EQ(llvm::support::endian::read32le(S.bytes(Returned, 4).data()), 4u);
  }
  checked(S.Memory->protect(Scratch, Page, Write));
  const auto Before = S.bytes();
  denied(S.query(Scratch, 4, Returned), "accessible ordinary RAM");
  EXPECT_EQ(S.bytes(), Before);
}

TEST(KernelVaShadowQuery, InvalidEitherOutputCannotPartiallyWriteTheOther) {
  Session S;
  S.initialize();
  S.prepare();
  const uint64_t Other = Scratch + 2 * Page;
  checked(S.Memory->map(Other, Page, Read));
  for (uint64_t Bad : {uint64_t(0), Scratch + Page - 3, UINT64_MAX - 2, Other,
                       Other + Page - 3}) {
    const auto Before = S.bytes();
    auto R = S.query(Bad);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    EXPECT_EQ(S.bytes(), Before);
    if (!Bad)
      continue; // NULL ReturnLength is explicitly valid.
    for (uint64_t Length : {0u, 3u, 4u, 8u}) {
      R = S.query(Scratch, Length, Bad);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(S.bytes(), Before);
    }
  }
  // A valid first page is insufficient when the next output byte is read-only.
  checked(S.Memory->map(Scratch + Page, Page, Read));
  const uint64_t Cross = Scratch + Page - 2;
  const auto Before = S.bytes(Cross, 4);
  denied(S.query(Cross), "accessible ordinary RAM");
  EXPECT_EQ(S.bytes(Cross, 4), Before);
  denied(S.query(Scratch, 4, Cross), "accessible ordinary RAM");
  EXPECT_EQ(S.bytes(Cross, 4), Before);
}

TEST(KernelVaShadowQuery, OverlappingDestinationsRejectBeforeAnyWrite) {
  Session S;
  S.initialize();
  for (int Offset = -3; Offset <= 3; ++Offset) {
    S.prepare();
    const auto Before = S.bytes();
    denied(S.query(Scratch + 64, 4, Scratch + 64 + Offset), "overlap");
    EXPECT_EQ(S.bytes(), Before);
  }
  EXPECT_EQ(checked(S.query(Scratch + 64, 4, Scratch + 68)), 0u);
  EXPECT_EQ(checked(S.Memory->readInteger(Scratch + 64, 4)), 0x1234u);
  EXPECT_EQ(checked(S.Memory->readInteger(Scratch + 68, 4)), 4u);
}

TEST(KernelVaShadowQuery, EitherOutputAliasRejectsUntilRetired) {
  for (bool AliasLength : {false, true})
    for (uint64_t Alias :
         {Scratch - Page, Scratch + 3 * Page, UINT64_MAX - Page + 1})
      for (unsigned Rights : {0u, unsigned(Read), unsigned(Read | Write)}) {
        Session S;
        S.initialize();
        S.prepare();
        const uint64_t Returned = Scratch + 2 * Page;
        checked(S.Memory->map(Returned, Page, Read | Write));
        checked(S.Memory->writeInteger(Returned, 0xdeadbeef, 4));
        const auto Before = S.bytes();
        const auto BeforeLength = S.bytes(Returned, 4);
        checked(S.Memory->mapAlias(Alias, AliasLength ? Returned : Scratch,
                                   Page, Rights));
        denied(S.query(Scratch, 4, Returned), "alias");
        EXPECT_EQ(S.bytes(), Before);
        EXPECT_EQ(S.bytes(Returned, 4), BeforeLength);
        checked(S.Memory->unmapAlias(Alias, Page));
        EXPECT_EQ(checked(S.query(Scratch, 4, Returned)), 0u);
      }
}

TEST(KernelVaShadowQuery, GuardedObjectsAndUserAliasesCannotBecomeOutput) {
  using namespace windows;
  Session S;
  S.initialize();
  const uint64_t Record = Base + Page;
  EXPECT_EQ(checked(S.Model.call("KeRegisterBugCheckReasonCallback",
                                 {Record, Base, 2, Scratch})),
            1u);
  const auto Original = S.bytes(Record, 48);
  denied(S.query(Record + 36), "opaque");
  denied(S.query(Scratch, 4, Record + 36), "read-only");
  EXPECT_EQ(S.bytes(Record, 48), Original);
  const uint64_t User = S.beginUserRequest();
  const uint64_t MDL =
      checked(S.Model.call("IoAllocateMdl", {User, Page, 0, 0, 0}));
  checked(S.Model.call("MmProbeAndLockPages", {MDL, UserMode, IoModifyAccess}));
  const uint64_t Alias = checked(
      S.Model.call("MmGetSystemAddressForMdlSafe", {MDL, NormalPagePriority}));
  checked(S.Model.call("KeInitializeEvent",
                       {User, dispatcher::NotificationObject, 1}));
  const auto Before = S.bytes(User);
  for (uint64_t Address : {User, Alias}) {
    for (bool IsLength : {false, true}) {
      auto R = IsLength ? S.query(Scratch, 4, Address) : S.query(Address);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(S.bytes(User), Before);
    }
  }
  EXPECT_EQ(checked(S.Model.call("KeReadStateEvent", {User})), 1u);
}

TEST(KernelVaShadowQuery, BorrowedFieldsCannotBecomeEitherOutput) {
  Session S;
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.LoaderView = DriverLoaderView{1, 0, "borrowed fields"};
  Options.KernelVaShadowQuery =
      DriverKernelVaShadowQuery{1, 42, "borrowed fields"};
  checked(S.Model.initialize(S.Image, Options));
  const uint64_t Loader =
      checked(S.Memory->readInteger(S.Model.driverObject() + 40, 8));
  S.prepare();
  const auto Before = S.bytes();
  const auto Borrowed = S.bytes(Loader, 160);
  denied(S.query(Loader + 48), "read-only");
  denied(S.query(Scratch, 4, Loader + 48), "read-only");
  EXPECT_EQ(S.bytes(), Before);
  EXPECT_EQ(S.bytes(Loader, 160), Borrowed);
}

TEST(KernelVaShadowQuery, DeviceMemoryAndFreedPoolsCannotPublishOutput) {
  Session S;
  S.initialize();
  S.prepare();
  unsigned Validations = 0, Reads = 0, Writes = 0;
  const uint64_t Device = Scratch + 2 * Page;
  GuestMMIOCallbacks Callbacks;
  Callbacks.Validate = [&](uint64_t, uint64_t, bool) {
    ++Validations;
    return llvm::Error::success();
  };
  Callbacks.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++Reads;
    return 0;
  };
  Callbacks.Write = [&](uint64_t, unsigned, uint64_t) {
    ++Writes;
    return llvm::Error::success();
  };
  checked(S.Memory->mapMMIO(Device, Page, std::move(Callbacks)));
  const uint64_t Pool = checked(S.Model.call("ExAllocatePool", {0, 16}));
  checked(S.Model.call("ExFreePool", {Pool}));
  const auto Before = S.bytes();
  for (uint64_t Bad : {Device, Pool})
    for (bool IsLength : {false, true}) {
      auto R = IsLength ? S.query(Scratch, 4, Bad) : S.query(Bad);
      ASSERT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
      EXPECT_EQ(S.bytes(), Before);
    }
  EXPECT_EQ(Validations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(Writes, 0u);
}

TEST(KernelVaShadowQuery, UniqueCrossPageOutputsAllowUnrelatedAliases) {
  Session S;
  S.initialize();
  checked(S.Memory->map(Scratch + Page, Page, Read | Write));
  checked(S.Memory->mapAlias(Scratch + 2 * Page, Base + Page, Page, Read));
  const uint64_t Cross = Scratch + Page - 2;
  for (bool LengthCrosses : {false, true}) {
    const uint64_t Out = LengthCrosses ? Scratch : Cross;
    const uint64_t Returned = LengthCrosses ? Cross : Scratch;
    EXPECT_EQ(checked(S.query(Out, 4, Returned)), 0u);
    EXPECT_EQ(checked(S.Memory->readInteger(Out, 4)), 0x1234u);
    EXPECT_EQ(checked(S.Memory->readInteger(Returned, 4)), 4u);
  }
}

TEST(KernelVaShadowQuery, CopiedDeclarationAndBaselineKeepDependency) {
  Session S;
  DriverOptions Options;
  Options.Contract = ExecutionContract::CheckedX64;
  Options.KernelVaShadowQuery = DriverKernelVaShadowQuery{1, 42, "original"};
  checked(S.Model.initialize(S.Image, Options));
  Options.KernelVaShadowQuery->Flags = 99;
  checked(S.Model.captureUnpackBaseline());
  const auto Before = checked(S.Model.unpackDependencies());
  EXPECT_NE(std::find(Before.begin(), Before.end(),
                      "unreconstructed kernel or environment effects"),
            Before.end());
  EXPECT_EQ(checked(S.query()), 0u);
  EXPECT_EQ(checked(S.Memory->readInteger(Scratch, 4)), 42u);
  EXPECT_EQ(checked(S.Model.unpackDependencies()), Before);
}

TEST(KernelVaShadowQuery, InvalidDeclarationsFailBeforeAllocation) {
  for (auto Query : {DriverKernelVaShadowQuery{0, 0, "bad"},
                     {2, 0, "bad"},
                     {1, std::nullopt, "missing"},
                     {1, 0x4000, "reserved"},
                     {1, 0x80000000, "reserved"},
                     {1, 0, ""},
                     {1, 0, std::string(257, 'x')},
                     {1, 0, "bad\nlabel"}}) {
    Session S;
    DriverOptions Options;
    Options.Contract = ExecutionContract::CheckedX64;
    Options.KernelVaShadowQuery = Query;
    denied(S.Model.initialize(S.Image, Options),
           "driver kernel_va_shadow_query:");
    EXPECT_EQ(S.Model.driverObject(), 0u);
    EXPECT_TRUE(S.Model.unpackAllocations().empty());
    S.initialize();
    EXPECT_NE(S.Model.driverObject(), 0u);
  }
  Session S;
  DriverOptions Legacy;
  Legacy.KernelVaShadowQuery = DriverKernelVaShadowQuery{1, 0, "legacy"};
  denied(S.Model.initialize(S.Image, Legacy), "checked-x64");
  EXPECT_TRUE(S.Model.unpackAllocations().empty());
}
} // namespace
} // namespace neverd::emulation
