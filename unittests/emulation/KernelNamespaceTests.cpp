//===- KernelNamespaceTests.cpp - Object namespace contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original namespace, buffer and reference-lifetime regressions.
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"

namespace neverd::emulation {
namespace {
class KernelNamespace : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000, Entry = 0x180001000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;

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
  void SetUp() override {
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    ok(Exports.initialize({}));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    DriverOptions Options;
    Options.ServiceName = "NamespaceFixture";
    ok(Model->initialize(Image, Options));
    Model->enterExecution(profile::StackBase);
  }
  uint64_t call(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    return take(Model->call(Name.str(), A));
  }
  std::string fail(llvm::StringRef Name, std::initializer_list<uint64_t> A) {
    auto V = Model->call(Name.str(), A);
    if (V) {
      ADD_FAILURE() << Name.str() << " unexpectedly succeeded";
      return {};
    }
    return llvm::toString(V.takeError());
  }
  uint64_t read(uint64_t Address, unsigned Size = 8) {
    return take(Memory->readInteger(Address, Size));
  }
  void write(uint64_t Address, uint64_t Value, unsigned Size = 8) {
    ok(Memory->writeInteger(Address, Value, Size));
  }
  void unicode(uint64_t Address, llvm::StringRef Text) {
    write(Address, Text.size() * 2, 2);
    write(Address + 2, (Text.size() + 1) * 2, 2);
    write(Address + 8, Address + 16);
    for (size_t I = 0; I <= Text.size(); ++I)
      write(Address + 16 + I * 2, I == Text.size() ? 0 : uint8_t(Text[I]), 2);
  }
  std::string string(uint64_t Record) {
    const auto Length = read(Record, 2), Buffer = read(Record + 8);
    std::string Out;
    for (size_t I = 0; I < Length; I += 2)
      Out.push_back(char(read(Buffer + I, 2)));
    return Out;
  }
  void attributes(llvm::StringRef Name, uint64_t Root = 0,
                  uint32_t Flags = 0x240) {
    unicode(Scratch + 0x100, Name);
    write(Scratch + 0x80, 48, 4);
    write(Scratch + 0x88, Root);
    write(Scratch + 0x90, Scratch + 0x100);
    write(Scratch + 0x98, Flags, 4);
    write(Scratch + 0xa0, 0);
    write(Scratch + 0xa8, 0);
  }
  uint64_t open(llvm::StringRef API, llvm::StringRef Name, uint64_t Root = 0,
                uint32_t Flags = 0x240, uint32_t Access = 1) {
    attributes(Name, Root, Flags);
    EXPECT_EQ(call(API, {Scratch, Access, Scratch + 0x80}), 0u);
    return read(Scratch);
  }
  uint64_t device(llvm::StringRef Name) {
    unicode(Scratch + 0x300, Name);
    EXPECT_EQ(call("IoCreateDevice",
                   {Result.DriverObject, 0, Name.empty() ? 0 : Scratch + 0x300,
                    0x22, 0x100, 0, Scratch}),
              0u);
    return read(Scratch);
  }
  void link(llvm::StringRef Name, llvm::StringRef Target) {
    unicode(Scratch + 0x300, Name);
    unicode(Scratch + 0x500, Target);
    EXPECT_EQ(call("IoCreateSymbolicLink", {Scratch + 0x300, Scratch + 0x500}),
              0u);
  }
  void eraseLink(llvm::StringRef Name) {
    unicode(Scratch + 0x300, Name);
    EXPECT_EQ(call("IoDeleteSymbolicLink", {Scratch + 0x300}), 0u);
  }
  std::string queryName(uint64_t Object) {
    EXPECT_EQ(
        call("ObQueryNameString", {Object, Scratch + 0x800, 512, Scratch + 8}),
        0u);
    return string(Scratch + 0x800);
  }
  std::string denied(uint64_t Object) {
    auto E = Model->validateGuestAccess(Object, 1, false);
    if (!E) {
      ADD_FAILURE() << "opaque object unexpectedly readable";
      return {};
    }
    return llvm::toString(std::move(E));
  }
};

TEST_F(KernelNamespace, DriverNamesUseObjectIdentityAndTypedReferences) {
  const auto Driver = Result.DriverObject;
  const auto Type = call("ObGetObjectType", {Driver});
  EXPECT_EQ(queryName(Driver), "\\Driver\\NamespaceFixture");
  // DRIVER_OBJECT.DriverName is a public mutable field, not the namespace
  // owner.
  write(Driver + 0x38, 0, 4);
  write(Driver + 0x40, 0);
  EXPECT_EQ(queryName(Driver), "\\Driver\\NamespaceFixture");
  unicode(Scratch + 0x100, "\\Driver\\namespacefixture");
  EXPECT_EQ(call("ObReferenceObjectByName",
                 {Scratch + 0x100, 0x40, 0, 0, Type, 0, 0, Scratch}),
            0u);
  EXPECT_EQ(read(Scratch), Driver);
  call("ObfDereferenceObject", {Driver});
  EXPECT_FALSE(fail("ObfDereferenceObject", {Driver}).empty());
  write(Scratch, 0xfeed);
  EXPECT_EQ(call("ObReferenceObjectByName",
                 {Scratch + 0x100, 0, 0, 0, Type, 0, 0, Scratch}),
            0xc0000034u);
  EXPECT_EQ(read(Scratch), 0u);
  auto Wrong = call("ObGetObjectType", {call("PsGetCurrentProcess", {})});
  EXPECT_EQ(call("ObReferenceObjectByName",
                 {Scratch + 0x100, 0x40, 0, 0, Wrong, 0, 0, Scratch}),
            0xc0000024u);
  EXPECT_EQ(read(Scratch), 0u);
  EXPECT_FALSE(fail("ObReferenceObjectByName",
                    {Scratch + 0x100, 0x440, 0, 0, Type, 0, 0, Scratch})
                   .empty());
  EXPECT_FALSE(fail("ObReferenceObjectByName",
                    {Scratch + 0x100, 0x40, 0, 0, Type, 1, 0, Scratch})
                   .empty());
}

TEST_F(KernelNamespace, NameQueriesPreserveShortBuffersAndUnnamedObjects) {
  const auto Driver = Result.DriverObject;
  write(Scratch + 0x800, 0x0123456789abcdef);
  EXPECT_EQ(call("ObQueryNameString", {Driver, 0, 0, Scratch}), 0xc0000004u);
  const auto Required = read(Scratch, 4);
  EXPECT_EQ(Required,
            16u + 2u * (std::string("\\Driver\\NamespaceFixture").size() + 1));
  EXPECT_EQ(call("ObQueryNameString",
                 {Driver, Scratch + 0x800, Required - 1, Scratch}),
            0xc0000004u);
  EXPECT_EQ(read(Scratch + 0x800), 0x0123456789abcdefu);
  EXPECT_EQ(
      call("ObQueryNameString", {Driver, Scratch + 0x800, Required, Scratch}),
      0u);
  EXPECT_EQ(read(Scratch + 0x804, 4), 0u);
  EXPECT_EQ(read(Scratch + 0x808), Scratch + 0x810);
  EXPECT_EQ(read(Scratch + 0x800 + Required - 2, 2), 0u);
  const auto Thread = call("PsGetCurrentThread", {});
  EXPECT_EQ(call("ObQueryNameString", {Thread, 0, 0, Scratch}), 0xc0000004u);
  EXPECT_EQ(read(Scratch, 4), 16u);
  EXPECT_EQ(call("ObQueryNameString", {Thread, Scratch + 0x800, 16, Scratch}),
            0u);
  EXPECT_EQ(read(Scratch + 0x800), 0u);
  EXPECT_EQ(read(Scratch + 0x808), 0u);
  EXPECT_FALSE(fail("ObQueryNameString", {Scratch, 0, 0, Scratch}).empty());
}

TEST_F(KernelNamespace, DirectoryRootsCasesAndErrorsUseRealHandleTypes) {
  const auto Root = open("ZwOpenDirectoryObject", "\\", 0, 0x240, 0x80000000);
  const auto Dir = open("ZwOpenDirectoryObject", "Driver", Root);
  EXPECT_EQ(call("ZwQueryObject", {Dir, 1, Scratch + 0x800, 512, 0}), 0u);
  EXPECT_EQ(string(Scratch + 0x800), "\\Driver");
  EXPECT_EQ(call("ObReferenceObjectByHandle",
                 {Root, 0x20003, 0, 0, Scratch, Scratch + 16}),
            0u);
  const auto Object = read(Scratch);
  EXPECT_EQ(
      call("ObQueryNameString", {Object, Scratch + 0x800, 64, Scratch + 0x800}),
      0u);
  EXPECT_EQ(read(Scratch + 0x800, 4), 0x00040002u);
  EXPECT_EQ(
      call("ZwQueryObject", {Root, 1, Scratch + 0x800, 64, Scratch + 0x800}),
      0u);
  EXPECT_EQ(read(Scratch + 0x800, 4), 20u);
  EXPECT_EQ(read(Scratch + 16, 4), 0u);
  EXPECT_EQ(read(Scratch + 20, 4), 0x20003u);
  EXPECT_EQ(queryName(Object), "\\");
  EXPECT_FALSE(denied(Object).empty());
  call("ObfDereferenceObject", {Object});
  for (const auto &[Name, Status] :
       std::vector<std::pair<std::string, uint32_t>>{
           {"Driver", 0xc000003b},
           {"\\driver", 0xc0000034},
           {"\\Missing", 0xc0000034},
           {"\\Missing\\Child", 0xc000003a},
           {"\\Driver\\", 0xc0000033},
           {"\\Driver\\NamespaceFixture", 0xc0000024}}) {
    attributes(Name, 0, 0x200);
    write(Scratch, 0xfeed);
    EXPECT_EQ(call("ZwOpenDirectoryObject", {Scratch, 1, Scratch + 0x80}),
              Status)
        << Name;
    EXPECT_EQ(read(Scratch), 0xfeedu);
  }
  attributes("Driver", 0x7777);
  EXPECT_EQ(call("ZwOpenDirectoryObject", {Scratch, 1, Scratch + 0x80}),
            0xc0000008u);
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {Dir, Scratch + 0x100, 0}),
            0xc0000024u);
  EXPECT_FALSE(
      fail("ZwQueryObject", {Dir, 0, Scratch + 0x800, 512, 0}).empty());
  EXPECT_EQ(call("ZwClose", {Dir}), 0u);
  EXPECT_EQ(call("ZwClose", {Root}), 0u);
}

TEST_F(KernelNamespace,
       SymbolicLinkQueriesCaptureMaximumLengthAndOptionalTail) {
  unicode(Scratch + 0x300, "\\DosDevices\\Example");
  unicode(Scratch + 0x500, "\\Device\\Target");
  const auto Length = read(Scratch + 0x500, 2);
  write(Scratch + 0x502, Length + 6, 2);
  write(Scratch + 0x510 + Length, 0x33221100, 4);
  write(Scratch + 0x514 + Length, 0x5566, 2);
  EXPECT_EQ(call("IoCreateSymbolicLink", {Scratch + 0x300, Scratch + 0x500}),
            0u);
  const auto Handle =
      open("ZwOpenSymbolicLinkObject", "\\??\\Example", 0, 0x240, 0x80000000);
  write(Scratch + 0x510 + Length, 0, 4);
  write(Scratch + 0x514 + Length, 0, 2); // Captured bytes are independent.
  unicode(Scratch + 0x800, "old");
  write(Scratch + 0x802, Length, 2);
  EXPECT_EQ(
      call("ZwQuerySymbolicLinkObject", {Handle, Scratch + 0x800, Scratch}),
      0xc0000023u);
  EXPECT_EQ(read(Scratch, 4), Length + 6);
  EXPECT_EQ(string(Scratch + 0x800), "old");
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {Handle, Scratch + 0x800, 0}),
            0u);
  EXPECT_EQ(string(Scratch + 0x800), "\\Device\\Target");
  write(Scratch + 0x802, Length + 6, 2);
  EXPECT_EQ(
      call("ZwQuerySymbolicLinkObject", {Handle, Scratch + 0x800, Scratch}),
      0u);
  EXPECT_EQ(read(Scratch + 0x810 + Length, 4), 0x33221100u);
  EXPECT_EQ(read(Scratch + 0x814 + Length, 2), 0x5566u);
  EXPECT_EQ(read(Scratch, 4), Length + 6);
  EXPECT_EQ(read(Scratch + 0x802, 2), Length + 6);
  eraseLink("\\??\\Example");
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
}

TEST_F(KernelNamespace,
       TemporaryLinkRetainsNameUntilLastHandleAndPointerAfterward) {
  link("\\??\\Example", "\\Device\\First");
  const auto First = open("ZwOpenSymbolicLinkObject", "\\GLOBAL??\\Example");
  EXPECT_EQ(call("ObReferenceObjectByHandle", {First, 0, 0, 0, Scratch, 0}),
            0u);
  const auto Object = read(Scratch);
  const auto Type = call("ObGetObjectType", {Object});
  eraseLink("\\DosDevices\\Example");
  const auto Second = open("ZwOpenSymbolicLinkObject", "\\??\\Example");
  unicode(Scratch + 0x300, "\\??\\Example");
  unicode(Scratch + 0x500, "\\Device\\New");
  EXPECT_EQ(call("IoCreateSymbolicLink", {Scratch + 0x300, Scratch + 0x500}),
            0xc0000035u);
  EXPECT_EQ(call("ZwClose", {First}), 0u);
  EXPECT_EQ(queryName(Object), "\\GLOBAL??\\Example");
  EXPECT_EQ(call("ZwClose", {Second}), 0u);
  EXPECT_EQ(queryName(Object), "\\Example");
  attributes("\\??\\Example");
  EXPECT_EQ(call("ZwOpenSymbolicLinkObject", {Scratch, 1, Scratch + 0x80}),
            0xc0000034u);
  link("\\??\\Example", "\\Device\\New");
  const auto New = open("ZwOpenSymbolicLinkObject", "\\??\\Example");
  EXPECT_EQ(call("ObReferenceObjectByHandle", {New, 0, Type, 0, Scratch, 0}),
            0u);
  const auto NewObject = read(Scratch);
  EXPECT_NE(NewObject, Object);
  call("ObfDereferenceObject", {NewObject});
  EXPECT_EQ(call("ObOpenObjectByPointer",
                 {Object, 0x200, 0, 0x80000000, Type, 0, Scratch}),
            0u);
  const auto Old = read(Scratch);
  unicode(Scratch + 0x800, "buffer with sufficient capacity");
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject", {Old, Scratch + 0x800, 0}), 0u);
  EXPECT_EQ(string(Scratch + 0x800), "\\Device\\First");
  call("ObfDereferenceObject", {Object});
  EXPECT_EQ(call("ZwClose", {Old}), 0u);
  EXPECT_FALSE(fail("ObfReferenceObject", {Object}).empty());
  eraseLink("\\??\\Example");
  EXPECT_EQ(call("ZwClose", {New}), 0u);
}

TEST_F(KernelNamespace, DeviceReferencesDelayStorageRetirementAndFollowLinks) {
  const auto Device = device("\\Device\\Target");
  const auto Type = call("ObGetObjectType", {Device});
  EXPECT_EQ(queryName(Device), "\\Device\\Target");
  link("\\??\\Example", "\\Device\\Target");
  unicode(Scratch + 0x100, "\\DosDevices\\Example");
  EXPECT_EQ(call("ObReferenceObjectByName",
                 {Scratch + 0x100, 0x40, 0, 0, Type, 0, 0, Scratch}),
            0u);
  EXPECT_EQ(read(Scratch), Device);
  call("IoDeleteDevice", {Device});
  EXPECT_EQ(read(Result.DriverObject + 8), 0u);
  ok(Model->snapshot());
  EXPECT_TRUE(Result.Devices.empty());
  const auto Replacement = device("\\Device\\Target");
  EXPECT_NE(Replacement, Device);
  call("IoDeleteDevice", {Replacement});
  EXPECT_EQ(queryName(Device), "\\Target");
  EXPECT_EQ(call("ObGetObjectType", {Device}), Type);
  EXPECT_EQ(call("ObReferenceObjectByName",
                 {Scratch + 0x100, 0x40, 0, 0, Type, 0, 0, Scratch}),
            0xc0000034u);
  call("ObfDereferenceObject", {Device});
  EXPECT_FALSE(fail("ObGetObjectType", {Device}).empty());
  EXPECT_FALSE(denied(Device).empty());
  eraseLink("\\??\\Example");
  const auto Unnamed = device("");
  EXPECT_EQ(queryName(Unnamed), "");
  call("IoDeleteDevice", {Unnamed});
}

TEST_F(KernelNamespace, HandleScopeAndUserModeRightsSurviveGenericMapping) {
  const auto Local =
      open("ZwOpenDirectoryObject", "\\Device", 0, 0x40, 0x80000000);
  EXPECT_EQ(call("ObReferenceObjectByHandle", {Local, 4, 0, 1, Scratch, 0}),
            0xc0000022u);
  EXPECT_EQ(call("ObReferenceObjectByHandle", {Local, 1, 0, 1, Scratch, 0}),
            0u);
  call("ObfDereferenceObject", {read(Scratch)});
  const auto Kernel = open("ZwOpenDirectoryObject", "\\Device");
  ok(Model->setUserRequestContext(true, 0x2000));
  EXPECT_EQ(call("ZwQueryObject", {Local, 1, Scratch + 0x800, 512, 0}),
            0xc0000008u);
  EXPECT_EQ(call("ZwClose", {Local}), 0xc0000008u);
  EXPECT_EQ(call("ZwQueryObject", {Kernel, 1, Scratch + 0x800, 512, 0}), 0u);
  EXPECT_EQ(call("ObReferenceObjectByHandle", {Kernel, 0, 0, 1, Scratch, 0}),
            0xc0000008u);
  attributes("\\Device", 0, 0x40);
  EXPECT_FALSE(
      fail("ZwOpenDirectoryObject", {Scratch, 1, Scratch + 0x80}).empty());
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {Local}), 0u);
  EXPECT_EQ(call("ZwClose", {Kernel}), 0u);
}

TEST_F(KernelNamespace, BadOutputPreflightAcquiresNoHandlesOrReferences) {
  constexpr uint64_t ReadOnly = Scratch + 0x1000;
  ok(Memory->protect(ReadOnly, 4096, Read));
  attributes("\\Driver");
  EXPECT_FALSE(
      fail("ZwOpenDirectoryObject", {ReadOnly, 1, Scratch + 0x80}).empty());
  EXPECT_FALSE(
      fail("ZwOpenDirectoryObject", {Scratch + 0xfffc, 1, Scratch + 0x80})
          .empty());
  EXPECT_EQ(call("ZwOpenDirectoryObject", {Scratch, 1, Scratch + 0x80}), 0u);
  const auto Handle = read(Scratch);
  EXPECT_EQ(Handle, 0x60000000u);
  const auto Driver = Result.DriverObject,
             Type = call("ObGetObjectType", {Driver});
  unicode(Scratch + 0x100, "\\Driver\\NamespaceFixture");
  EXPECT_FALSE(fail("ObReferenceObjectByName",
                    {Scratch + 0x100, 0x40, 0, 0, Type, 0, 0, ReadOnly})
                   .empty());
  EXPECT_FALSE(fail("ObfDereferenceObject", {Driver}).empty());
  write(Scratch + 8, 0xfeed, 4);
  EXPECT_FALSE(
      fail("ObQueryNameString", {Driver, ReadOnly, 512, Scratch + 8}).empty());
  EXPECT_EQ(read(Scratch + 8, 4), 0xfeedu);
  EXPECT_FALSE(
      fail("ObQueryNameString", {Driver, Scratch + 0x800, 512, ReadOnly})
          .empty());
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
}
TEST_F(KernelNamespace, LinkOutputPermissionsAndAliasingFollowCopyOrder) {
  link("\\??\\Example", "\\Device\\Target");
  const auto Handle = open("ZwOpenSymbolicLinkObject", "\\??\\Example");
  constexpr auto ReadOnly = Scratch + 0x1000;
  ok(Memory->protect(ReadOnly, 4096, Read));
  write(Scratch + 0x800, 7, 2);
  write(Scratch + 0x802, 0, 2);
  write(Scratch + 0x808, ReadOnly);
  EXPECT_EQ(
      call("ZwQuerySymbolicLinkObject", {Handle, Scratch + 0x800, Scratch}),
      0xc0000023u);
  EXPECT_EQ(read(Scratch + 0x800, 2), 7u);
  write(Scratch + 0x802, 64, 2);
  write(Scratch, 0xfeed, 4);
  EXPECT_FALSE(
      fail("ZwQuerySymbolicLinkObject", {Handle, Scratch + 0x800, Scratch})
          .empty());
  EXPECT_EQ(read(Scratch, 4), 0xfeedu);
  EXPECT_EQ(read(Scratch + 0x800, 2), 7u);
  write(Scratch + 0x808, Scratch + 0x900);
  EXPECT_FALSE(
      fail("ZwQuerySymbolicLinkObject", {Handle, Scratch + 0x800, ReadOnly})
          .empty());
  EXPECT_EQ(read(Scratch + 0x900), 0u);
  // ReturnedLength is written after the string length; overlapping outputs
  // observe that documented syscall order, without acquiring extra references.
  EXPECT_EQ(call("ZwQuerySymbolicLinkObject",
                 {Handle, Scratch + 0x800, Scratch + 0x800}),
            0u);
  EXPECT_EQ(read(Scratch + 0x800, 4), 30u);
  EXPECT_EQ(read(Scratch + 0x91c, 2), 0u);
  eraseLink("\\??\\Example");
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
}

TEST_F(KernelNamespace, NameQueryIRQLAndUnsupportedSecurityAreExplicit) {
  attributes("\\Driver");
  for (uint32_t Flags : {0x240u, 0x640u}) {
    write(Scratch + 0x98, Flags, 4);
    if (Flags == 0x240)
      write(Scratch + 0xa0, Scratch + 0x200);
    EXPECT_FALSE(
        fail("ZwOpenDirectoryObject", {Scratch, 1, Scratch + 0x80}).empty());
    write(Scratch + 0xa0, 0);
  }
  attributes("\\Driver");
  EXPECT_FALSE(
      fail("ZwOpenDirectoryObject", {Scratch, 0x2000000, Scratch + 0x80})
          .empty());
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  EXPECT_EQ(queryName(Result.DriverObject), "\\Driver\\NamespaceFixture");
  EXPECT_FALSE(
      fail("ZwOpenDirectoryObject", {Scratch, 1, Scratch + 0x80}).empty());
  call("KeLowerIrql", {0});
  EXPECT_EQ(call("KfRaiseIrql", {2}), 0u);
  EXPECT_FALSE(
      fail("ObQueryNameString", {Result.DriverObject, 0, 0, Scratch}).empty());
  call("KeLowerIrql", {0});
}
} // namespace
} // namespace neverd::emulation
