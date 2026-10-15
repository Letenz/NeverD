//===- KernelDataExportTests.cpp - Kernel data identity and access -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validate data identities, access boundaries and recovery observations.
///
//===----------------------------------------------------------------------===//
#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelDataExports.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"

#include <array>
#include <set>
#include <string_view>

namespace neverd::emulation {
namespace {
class KernelDataExports : public testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<KernelModel> Model;
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
  void start(DriverOptions Options = {}) {
    Memory = take(UnicornBackend::create(4 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 4096, Read | Write));
    ok(Exports.initialize(Options));
    Model = std::make_unique<KernelModel>(*Memory, Result, &Exports);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    Model->enterExecution(profile::StackBase);
  }
  uint64_t address(const char *Name) { return take(Exports.resolve(Name)); }
  uint64_t read(const char *Name, unsigned Size = 8) {
    return take(Memory->readInteger(address(Name), Size));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> A = {}) {
    return take(Model->call(Name, A));
  }
};

TEST_F(KernelDataExports, TypesAndSystemProcessShareTheObjectOwner) {
  start();
  const uint64_t Process = call("PsGetCurrentProcess");
  const uint64_t Thread = call("PsGetCurrentThread");
  EXPECT_EQ(read("PsInitialSystemProcess"), Process);
  EXPECT_EQ(read("PsProcessType"), call("ObGetObjectType", {Process}));
  EXPECT_EQ(read("PsThreadType"), call("ObGetObjectType", {Thread}));
  EXPECT_EQ(read("IoDriverObjectType"),
            call("ObGetObjectType", {Model->driverObject()}));
  std::set<uint64_t> Types;
  for (const char *Name :
       {"PsProcessType", "PsThreadType", "IoDriverObjectType",
        "IoFileObjectType", "ExEventObjectType", "ExSemaphoreObjectType",
        "PsJobType", "SeTokenObjectType"}) {
    const auto Type = read(Name);
    EXPECT_NE(Type, 0u);
    EXPECT_TRUE(Types.insert(Type).second);
    EXPECT_NE(llvm::toString(Model->validateGuestAccess(Type, 1, false))
                  .find("opaque object type"),
              std::string::npos);
    if (std::string_view(Name) != "PsProcessType")
      EXPECT_EQ(call("ObReferenceObjectByPointer", {Process, 0, Type, 0}),
                0xc0000024u);
  }
  EXPECT_EQ(read("KeNumberProcessors", 1), 1u);
  EXPECT_EQ(call("ObReferenceObjectByPointer",
                 {Model->driverObject(), 0, read("IoDriverObjectType"), 0}),
            0u);
  call("ObfDereferenceObject", {Model->driverObject()});
  auto Handle = Model->call("ObOpenObjectByPointer",
                            {Model->driverObject(), 0x200, 0, 0,
                             read("IoDriverObjectType"), 0, Scratch});
  ASSERT_FALSE(bool(Handle));
  EXPECT_NE(llvm::toString(Handle.takeError()).find("object handle class"),
            std::string::npos);
  ok(Model->setUserRequestContext(true, 0x2000));
  EXPECT_NE(call("PsGetCurrentProcess"), Process);
  EXPECT_EQ(read("PsInitialSystemProcess"), Process);
  EXPECT_EQ(call("PsGetProcessId", {Process}), 4u);
  EXPECT_EQ(call("ObGetObjectType", {call("PsGetCurrentProcess")}),
            read("PsProcessType"));
}

TEST_F(KernelDataExports,
       ImportedAndDynamicAddressesAreReadOnlyAndNonExecutable) {
  DriverOptions Options;
  Options.DebuggerState = DriverDebuggerState{true, false};
  start(Options);
  for (const auto &Data : kernel_data::Exports) {
    const auto Address = address(Data.Name.data());
    EXPECT_EQ(
        take(Exports.bindImport({Scratch, "NTKRNLMP.EXE", Data.Name.str()})),
        Address);
    const auto *Entry = Exports.lookup(Address);
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, KernelExportRegistry::ExportKind::ModuleData);
    EXPECT_FALSE(KernelModel::argumentCount(*Entry));
    auto Called = Model->call(*Entry, {}, {});
    ASSERT_FALSE(bool(Called));
    EXPECT_NE(llvm::toString(Called.takeError()).find("not callable"),
              std::string::npos);
    EXPECT_TRUE(take(Memory->canAccess(Address, Data.Size, Read)));
    EXPECT_FALSE(take(Memory->canAccess(Address, Data.Size, Write)));
    EXPECT_FALSE(take(Memory->canAccess(Address, Data.Size, Execute)));
    ok(Model->validateGuestAccess(Address, Data.Size, false));
    EXPECT_NE(
        llvm::toString(Model->validateGuestAccess(Address, Data.Size, true))
            .find("read-only"),
        std::string::npos);
    auto Written = Model->call("RtlZeroMemory", {Address, Data.Size});
    ASSERT_FALSE(bool(Written));
    llvm::consumeError(Written.takeError());
  }
  EXPECT_EQ(read("KdDebuggerEnabled", 1), 1u);
  EXPECT_EQ(read("KdDebuggerNotPresent", 1), 0u);
}

TEST_F(KernelDataExports, UndeclaredDebuggerFactsAreNotObservableZeros) {
  start();
  for (const char *Name : {"KdDebuggerEnabled", "KdDebuggerNotPresent"}) {
    const auto Address = address(Name);
    EXPECT_NE(llvm::toString(Model->validateGuestAccess(Address, 1, false))
                  .find("debugger_state"),
              std::string::npos);
    EXPECT_FALSE(take(Memory->canAccess(Address, 1, Read)));
    EXPECT_FALSE(take(Memory->canAccess(Address, 1, Write)));
    EXPECT_FALSE(take(Memory->canAccess(Address, 1, Execute)));
    auto Copied = Model->call("RtlCopyMemory", {Scratch, Address, 1});
    ASSERT_FALSE(bool(Copied));
    EXPECT_NE(llvm::toString(Copied.takeError()).find("debugger_state"),
              std::string::npos);
  }
  const auto Debugger = address("KdDebuggerEnabled");
  EXPECT_NE(llvm::toString(Model->validateGuestAccess(Debugger - 1, 2, false))
                .find("debugger_state"),
            std::string::npos);
}

TEST_F(KernelDataExports, ReadsRetainRecoveryDependencyButPreflightIsPure) {
  start();
  ok(Model->captureUnpackBaseline());
  const auto Count = address("KeNumberProcessors");
  ok(Model->validateGuestAccess(Count, 1, false));
  EXPECT_TRUE(take(Memory->canAccess(Count, 1, Read)));
  std::array<uint8_t, 1> Bytes{};
  ok(Memory->snapshotBacking(Count, Bytes));
  EXPECT_TRUE(take(Model->unpackDependencies()).empty());
  ok(Memory->writeInteger(Scratch, 1, 1));
  EXPECT_EQ(call("RtlCompareMemory", {Scratch, Count, 1}), 1u);
  EXPECT_FALSE(take(Model->unpackDependencies()).empty());
  // Consuming just the scalar still depends on the declared CPU topology.
  EXPECT_EQ(call("RtlCopyMemory", {Scratch, Count, 1}), 0u);
}

TEST_F(KernelDataExports, DeclaredDebuggerFactsAreExplicitEvenWithoutReads) {
  DriverOptions Options;
  Options.DebuggerState = DriverDebuggerState{false, true};
  start(Options);
  EXPECT_EQ(read("KdDebuggerEnabled", 1), 0u);
  EXPECT_EQ(read("KdDebuggerNotPresent", 1), 1u);
  ok(Model->captureUnpackBaseline());
  EXPECT_FALSE(take(Model->unpackDependencies()).empty());
}

TEST_F(KernelDataExports, NestedStringBuffersRetainTheActualDataRead) {
  start();
  ok(Model->captureUnpackBaseline());
  // Neither API argument points into the data area. Its counted input does.
  for (uint64_t Record : {Scratch, Scratch + 16}) {
    ok(Memory->writeInteger(Record, 2, 2));
    ok(Memory->writeInteger(Record + 2, 2, 2));
  }
  ok(Memory->writeInteger(Scratch + 8, address("KeNumberProcessors"), 8));
  ok(Memory->writeInteger(Scratch + 24, Scratch + 64, 8));
  ok(Memory->writeInteger(Scratch + 64, 1, 2));
  EXPECT_TRUE(take(Model->unpackDependencies()).empty());
  EXPECT_EQ(call("RtlEqualUnicodeString", {Scratch, Scratch + 16, 0}), 1u);
  EXPECT_FALSE(take(Model->unpackDependencies()).empty());
}

TEST_F(KernelDataExports,
       AvailabilityDoesNotConsumeThunksOrCrossModuleBoundaries) {
  DriverOptions Options;
  Options.KernelExports = {{"PsProcessType", false}};
  start(Options);
  EXPECT_EQ(address("PsProcessType"), 0u);
  auto Missing = Exports.bindImport({Scratch, "ntoskrnl.exe", "PsProcessType"});
  ASSERT_FALSE(bool(Missing));
  llvm::consumeError(Missing.takeError());
  KernelExportRegistry Default;
  ok(Default.initialize({}));
  EXPECT_EQ(Exports.availableThunkCount(), Default.availableThunkCount());
  EXPECT_EQ(address("DbgPrint"), take(Default.resolve("DbgPrint")));
  const auto Other =
      take(Exports.bindImport({Scratch, "fltmgr.sys", "PsProcessType"}));
  ASSERT_NE(Exports.lookup(Other), nullptr);
  EXPECT_EQ(Exports.lookup(Other)->Kind,
            KernelExportRegistry::ExportKind::ModuleExport);
  EXPECT_EQ(address("PsProcessType"), 0u);
}
} // namespace
} // namespace neverd::emulation
