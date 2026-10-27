//===- KernelObjectTests.cpp - Object identity and lifetime contracts ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelExportRegistry.h"
#include "os/windows/kernel/KernelModel.h"

namespace neverd::emulation {
namespace {
class KernelObjects : public testing::Test {
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
    ok(Model->initialize(Image, {}));
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
  std::string denied(uint64_t Object) {
    auto E = Model->validateGuestAccess(Object, 1, false);
    if (!E) {
      ADD_FAILURE() << "opaque object unexpectedly readable";
      return {};
    }
    return llvm::toString(std::move(E));
  }
};

TEST_F(KernelObjects, FilterVersionHasExactArityAndPagedCodeAdmission) {
  EXPECT_EQ(call("ObGetFilterVersion", {}), 0x100u);
  EXPECT_FALSE(fail("ObGetFilterVersion", {0}).empty());
  ok(Model->setUserRequestContext(true, 0x2000));
  EXPECT_EQ(call("ObGetFilterVersion", {}), 0x100u);
  EXPECT_EQ(call("KfRaiseIrql", {1}), 0u);
  EXPECT_EQ(call("ObGetFilterVersion", {}), 0x100u);
  EXPECT_EQ(call("KfRaiseIrql", {2}), 1u);
  EXPECT_NE(fail("ObGetFilterVersion", {}).find("IRQL"), std::string::npos);
  call("KeLowerIrql", {1});
  EXPECT_EQ(call("ObGetFilterVersion", {}), 0x100u);
  call("KeLowerIrql", {0});
  EXPECT_FALSE(fail("ObRegisterCallbacks", {0, 0}).empty());
}

TEST_F(KernelObjects, LookupDoesNotInventProcessesOrOverwriteUnknownIDs) {
  write(Scratch, 0x11223344);
  for (const uint64_t ID : {0ULL, 8ULL, 0x1000ULL, 0x100000004ULL}) {
    EXPECT_EQ(call("PsLookupProcessByProcessId", {ID, Scratch}), 0xc000000bu);
    EXPECT_EQ(read(Scratch), 0x11223344u);
  }
  EXPECT_EQ(call("PsLookupProcessByProcessId", {4, Scratch}), 0u);
  const auto Process = read(Scratch);
  EXPECT_EQ(call("PsGetCurrentProcess", {}), Process);
  EXPECT_EQ(call("PsGetProcessId", {Process}), 4u);
  EXPECT_EQ(call("PsLookupProcessByProcessId", {4, Scratch + 8}), 0u);
  EXPECT_EQ(read(Scratch + 8), Process);
  call("ObfDereferenceObject", {Process});
  call("ObfDereferenceObject", {Process});
  EXPECT_FALSE(fail("ObfDereferenceObject", {Process}).empty());
  EXPECT_NE(denied(Process).find("opaque process"), std::string::npos);
  ok(Model->setUserRequestContext(true, 0x2000));
  EXPECT_EQ(call("PsLookupProcessByProcessId", {0x2000, Scratch}), 0u);
  const auto Requestor = read(Scratch);
  EXPECT_NE(Requestor, Process);
  EXPECT_EQ(call("PsGetCurrentProcess", {}), Requestor);
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("PsGetProcessId", {Requestor}), 0x2000u);
  call("ObfDereferenceObject", {Requestor});
}

TEST_F(KernelObjects, TypedPointerReferencesShareOneLifetimeInBothModes) {
  const auto Thread = call("PsGetCurrentThread", {});
  const auto Process = call("PsGetCurrentProcess", {});
  const auto ThreadType = call("ObGetObjectType", {Thread});
  const auto ProcessType = call("ObGetObjectType", {Process});
  ASSERT_NE(ThreadType, ProcessType);
  EXPECT_EQ(call("ObGetObjectType", {Thread}), ThreadType);
  EXPECT_NE(denied(ThreadType).find("opaque object type"), std::string::npos);
  for (uint64_t Mode : {0, 1}) {
    EXPECT_EQ(call("ObReferenceObjectByPointer",
                   {Thread, 0xffffffff, ThreadType, Mode}),
              0u);
    EXPECT_EQ(
        call("ObReferenceObjectByPointer", {Thread, 0, ProcessType, Mode}),
        0xc0000024u);
    call("ObfDereferenceObject", {Thread});
    EXPECT_FALSE(fail("ObfDereferenceObject", {Thread}).empty());
  }
  EXPECT_EQ(call("ObReferenceObjectByPointer", {Process, 0, 0, 1}),
            0xc0000024u);
  EXPECT_EQ(call("ObReferenceObjectByPointer", {Process, 0, 0, 0}), 0u);
  call("ObfReferenceObject", {Process});
  call("ObfDereferenceObject", {Process});
  call("ObfDereferenceObject", {Process});
  EXPECT_FALSE(fail("ObfDereferenceObject", {Process}).empty());
  EXPECT_FALSE(fail("ObfReferenceObject", {Scratch}).empty());
}

TEST_F(KernelObjects, HandlesRetainGrantedAccessScopeAndObjectType) {
  const auto Thread = call("PsGetCurrentThread", {});
  const auto Type = call("ObGetObjectType", {Thread});
  const auto Process = call("PsGetCurrentProcess", {});
  const auto WrongType = call("ObGetObjectType", {Process});
  EXPECT_EQ(call("ObOpenObjectByPointer", {Thread, 0, 0, 8, Type, 0, Scratch}),
            0u);
  const auto Local = read(Scratch);
  EXPECT_EQ(call("ObOpenObjectByPointer", {Thread, 0x200, 0, 0, 0, 0, Scratch}),
            0u);
  const auto Kernel = read(Scratch);
  EXPECT_NE(Local, Kernel);
  EXPECT_EQ(call("ObReferenceObjectByHandle",
                 {Local, 0, WrongType, 0, Scratch + 8, 0}),
            0xc0000024u);
  EXPECT_EQ(
      call("ObReferenceObjectByHandle", {Local, 0x10, Type, 1, Scratch + 8, 0}),
      0xc0000022u);
  EXPECT_EQ(
      call("ObReferenceObjectByHandle", {Kernel, 0, Type, 1, Scratch + 8, 0}),
      0xc0000008u);
  EXPECT_EQ(call("ObReferenceObjectByHandle",
                 {Local, 8, Type, 1, Scratch + 8, Scratch + 16}),
            0u);
  EXPECT_EQ(read(Scratch + 8), Thread);
  EXPECT_EQ(read(Scratch + 16, 4), 0u);
  EXPECT_EQ(read(Scratch + 20, 4), 8u);
  call("ObfDereferenceObject", {Thread});
  // Kernel mode bypasses the granted mask; it does not lose the handle's
  // original granted rights or the target object's type.
  EXPECT_EQ(
      call("ObReferenceObjectByHandle", {Local, 0x10, Type, 0, Scratch + 8, 0}),
      0u);
  call("ObfDereferenceObject", {Thread});
  ok(Model->setUserRequestContext(true, 0x2000));
  EXPECT_EQ(call("ObReferenceObjectByHandle", {Local, 0, 0, 0, Scratch + 8, 0}),
            0xc0000008u);
  EXPECT_EQ(call("ZwClose", {Local}), 0xc0000008u);
  EXPECT_EQ(call("ObReferenceObjectByHandle",
                 {Kernel, 0, Type, 0, Scratch + 8, Scratch + 16}),
            0u);
  EXPECT_EQ(read(Scratch + 16), 0u);
  call("ObfDereferenceObject", {Thread});
  EXPECT_EQ(call("ZwClose", {Kernel}), 0u);
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("ZwClose", {Local}), 0u);
  EXPECT_EQ(call("ZwClose", {Local}), 0xc0000008u);
  EXPECT_FALSE(fail("ObfDereferenceObject", {Thread}).empty());
}

TEST_F(KernelObjects, FailedOutputsDoNotAcquireReferencesOrConsumeHandles) {
  const auto Thread = call("PsGetCurrentThread", {});
  constexpr uint64_t ReadOnly = Scratch + 0x1000;
  write(Scratch, 0x12345678);
  ok(Memory->protect(ReadOnly, 4096, Read));
  for (uint64_t Output : {ReadOnly, Scratch + 0xfffc})
    EXPECT_FALSE(
        fail("ObOpenObjectByPointer", {Thread, 0, 0, 8, 0, 0, Output}).empty());
  EXPECT_EQ(call("ObOpenObjectByPointer", {Thread, 0, 0, 8, 0, 0, Scratch}),
            0u);
  const auto Handle = read(Scratch);
  EXPECT_EQ(Handle, 0x60000000u);
  write(Scratch + 8, 0xabcdef);
  EXPECT_FALSE(fail("ObReferenceObjectByHandle",
                    {Handle, 0, 0, 0, Scratch + 8, ReadOnly})
                   .empty());
  EXPECT_EQ(read(Scratch + 8), 0xabcdefu);
  EXPECT_FALSE(fail("ObfDereferenceObject", {Thread}).empty());
  const auto ID = call("PsGetThreadId", {Thread});
  EXPECT_FALSE(fail("PsLookupThreadByThreadId", {ID, ReadOnly}).empty());
  EXPECT_FALSE(fail("ObfDereferenceObject", {Thread}).empty());
  EXPECT_FALSE(fail("PsCreateSystemThread",
                    {Scratch + 8, 0x1fffff, 0, 0, ReadOnly, Entry, 0})
                   .empty());
  EXPECT_EQ(read(Scratch + 8), 0xabcdefu);
  EXPECT_FALSE(take(Model->nextScheduled(false)));
  EXPECT_EQ(call("ZwClose", {Handle}), 0u);
}

TEST_F(KernelObjects, PointerOpenClearsOutputBeforeTypeMismatch) {
  const auto Thread = call("PsGetCurrentThread", {});
  const auto WrongType =
      call("ObGetObjectType", {call("PsGetCurrentProcess", {})});
  write(Scratch, 0xabcdef);
  EXPECT_EQ(call("ObOpenObjectByPointer",
                 {Thread, 0x200, 0, 0, WrongType, 0, Scratch}),
            0xc0000024u);
  EXPECT_EQ(read(Scratch), 0u);
  EXPECT_EQ(call("ObOpenObjectByPointer", {Thread, 0x200, 0, 0, 0, 0, Scratch}),
            0u);
  EXPECT_EQ(read(Scratch), 0x60000000u);
  EXPECT_EQ(call("ZwClose", {read(Scratch)}), 0u);
  EXPECT_FALSE(fail("ObfDereferenceObject", {Thread}).empty());
}

TEST_F(KernelObjects, DetachedStackReuseKeepsOldIdentityAndExitPriority) {
  constexpr uint64_t Stack = Scratch + 0x8000;
  Model->enterExecution(Stack);
  const auto Old = call("PsGetCurrentThread", {});
  const auto ID = call("PsGetThreadId", {Old});
  call("KeSetPriorityThread", {Old, 13});
  call("ObfReferenceObject", {Old});
  EXPECT_EQ(call("ObOpenObjectByPointer", {Old, 0x200, 0, 0, 0, 0, Scratch}),
            0u);
  const auto Handle = read(Scratch);
  Model->enterExecution(profile::StackBase);
  EXPECT_EQ(call("KeWaitForSingleObject", {Old, 0, 0, 0, 0}), 0u);
  auto Wait = Model->takeWait();
  ASSERT_TRUE(Wait);
  ok(Model->retireStack(Stack, 4096));
  EXPECT_EQ(call("PsIsThreadTerminating", {Old}), 1u);
  EXPECT_EQ(call("KeQueryPriorityThread", {Old}), 13u);
  EXPECT_FALSE(fail("KeSetPriorityThread", {Old, 8}).empty());
  ok(Model->activateStack(Stack, 4096));
  Model->enterExecution(Stack);
  const auto New = call("PsGetCurrentThread", {});
  EXPECT_NE(Old, New);
  EXPECT_NE(call("PsGetThreadId", {New}), ID);
  EXPECT_EQ(call("KeQueryPriorityThread", {New}), 8u);
  EXPECT_EQ(call("KeQueryPriorityThread", {Old}), 13u);
  // The reused logical key is not a self-wait on the already exited object.
  write(Scratch + 0x20, 0);
  EXPECT_EQ(call("KeWaitForSingleObject", {Old, 0, 0, 0, Scratch + 0x20}), 0u);
  EXPECT_FALSE(Model->takeWait());
  // CID lookup remains valid for the exited object while references retain it.
  EXPECT_EQ(call("PsLookupThreadByThreadId", {ID, Scratch + 8}), 0u);
  EXPECT_EQ(read(Scratch + 8), Old);
  call("ObfDereferenceObject", {Old});
  call("ObfDereferenceObject", {Old});
  call("ZwClose", {Handle});
  EXPECT_NE(denied(Old).find("opaque thread"), std::string::npos);
  EXPECT_EQ(take(Model->pollWait(*Wait)), std::optional<uint32_t>(0));
  EXPECT_NE(denied(Old).find("freed"), std::string::npos);
  EXPECT_EQ(call("PsLookupThreadByThreadId", {ID, Scratch + 8}), 0xc000000bu);
  EXPECT_FALSE(fail("ObfReferenceObject", {Old}).empty());
  EXPECT_EQ(call("PsIsThreadTerminating", {New}), 0u);
}

TEST_F(KernelObjects, RetainedForegroundThreadDoesNotChangeRequestorIdentity) {
  ok(Model->setUserRequestContext(true, 0x2000));
  const auto Old = call("PsGetCurrentThread", {});
  const auto Process = call("PsGetThreadProcess", {Old});
  EXPECT_EQ(call("PsGetProcessId", {Process}), 0x2000u);
  call("ObfReferenceObject", {Old});
  ok(Model->setUserRequestContext(true, 0x3000));
  const auto New = call("PsGetCurrentThread", {});
  EXPECT_NE(New, Old);
  EXPECT_EQ(call("PsGetThreadProcess", {Old}), Process);
  EXPECT_EQ(call("IoThreadToProcess", {Old}), Process);
  EXPECT_EQ(call("PsGetProcessId", {call("PsGetThreadProcess", {New})}),
            0x3000u);
  call("ObfDereferenceObject", {Old});
  EXPECT_NE(denied(Old).find("freed"), std::string::npos);
  EXPECT_FALSE(fail("IoThreadToProcess", {Old}).empty());
}

TEST_F(KernelObjects, ThreadQueriesShareCreatingProcessAndPreviousMode) {
  const auto System = call("PsGetCurrentProcess", {});
  const auto SystemThread = call("PsGetCurrentThread", {});
  EXPECT_EQ(call("IoThreadToProcess", {SystemThread}), System);
  EXPECT_EQ(call("PsGetCurrentThreadPreviousMode", {}), 0u);
  ok(Model->setUserRequestContext(true, 0x2000));
  const auto Thread = call("PsGetCurrentThread", {});
  const auto Process = call("PsGetCurrentProcess", {});
  call("ObfReferenceObject", {Thread});
  EXPECT_NE(Process, System);
  EXPECT_EQ(call("IoThreadToProcess", {Thread}), Process);
  EXPECT_EQ(call("PsGetCurrentThreadPreviousMode", {}), 1u);
  EXPECT_EQ(call("PsGetCurrentThreadPreviousMode", {}),
            call("ExGetPreviousMode", {}));
  ok(Model->setUserRequestContext(false));
  EXPECT_EQ(call("PsGetCurrentThreadPreviousMode", {}), 0u);
  EXPECT_NE(call("PsGetCurrentThread", {}), Thread);
  EXPECT_EQ(call("IoThreadToProcess", {Thread}), Process);
  call("ObfDereferenceObject", {Thread});
  EXPECT_FALSE(fail("IoThreadToProcess", {Thread}).empty());
}

TEST_F(KernelObjects, ThreadQueriesCheckArityIdentityAndDispatchAdmission) {
  const auto Thread = call("PsGetCurrentThread", {});
  const auto Process = call("PsGetCurrentProcess", {});
  EXPECT_FALSE(fail("IoThreadToProcess", {}).empty());
  EXPECT_FALSE(fail("IoThreadToProcess", {Process}).empty());
  EXPECT_FALSE(fail("IoThreadToProcess", {Scratch}).empty());
  EXPECT_FALSE(fail("PsGetCurrentThreadPreviousMode", {0}).empty());
  call("KfRaiseIrql", {2});
  EXPECT_EQ(call("IoThreadToProcess", {Thread}), Process);
  EXPECT_EQ(call("PsGetCurrentThreadPreviousMode", {}), 0u);
  EXPECT_EQ(call("ExGetPreviousMode", {}), 0u);
  call("KfRaiseIrql", {3});
  EXPECT_NE(fail("IoThreadToProcess", {Thread}).find("IRQL"),
            std::string::npos);
  for (const char *Name :
       {"ExGetPreviousMode", "PsGetCurrentThreadPreviousMode"})
    EXPECT_NE(fail(Name, {}).find("IRQL"), std::string::npos);
}

TEST_F(KernelObjects, PcLookupUsesCompleteModuleExtentsWithoutReadingPc) {
  const uint64_t Output = Scratch + 0x10000 - 8;
  for (const char *Name : {"ntoskrnl.exe", "hal.dll"}) {
    auto Base = KernelExportRegistry::moduleBase(Name);
    ASSERT_TRUE(Base);
    // The independently published PE header must agree with module queries.
    const auto PE = *Base + read(*Base + 0x3c, 4);
    const uint64_t Size = read(PE + 0x50, 4);
    ASSERT_GT(Size, 0u);
    ASSERT_LT(Size, profile::KernelModuleStride);
    const uint64_t PCs[] = {*Base, *Base + 1, *Base + Size / 2,
                            *Base + Size - 1};
    for (auto PC : PCs) {
      write(Output, UINT64_MAX);
      EXPECT_EQ(call("RtlPcToFileHeader", {PC, Output}), *Base);
      EXPECT_EQ(read(Output), *Base);
    }
    EXPECT_EQ(call("RtlPcToFileHeader", {*Base + Size, Output}), 0u);
    EXPECT_EQ(read(Output), 0u);
  }
  // A reserved provider address is not a loaded module without its imports.
  const auto Unloaded = KernelExportRegistry::moduleBase("WDFLDR.SYS");
  ASSERT_TRUE(Unloaded);
  EXPECT_FALSE(take(Memory->canAccess(*Unloaded, 1, Read)));
  EXPECT_EQ(call("RtlPcToFileHeader", {*Unloaded, Output}), 0u);
  EXPECT_EQ(read(Output), 0u);
  constexpr uint64_t Image = 0x180000000;
  EXPECT_FALSE(take(Memory->canAccess(Image, 1, Read)));
  for (uint64_t PC : {Image, Image + 1, Image + 0x1000, Image + 0x2fff}) {
    EXPECT_EQ(call("RtlPcToFileHeader", {PC, Output}), Image);
    EXPECT_EQ(read(Output), Image);
  }
  for (uint64_t PC : {uint64_t(0), uint64_t(1), Image - 1, Image + 0x3000,
                      uint64_t(UINT64_MAX)}) {
    write(Output, UINT64_MAX);
    EXPECT_EQ(call("RtlPcToFileHeader", {PC, Output}), 0u);
    EXPECT_EQ(read(Output), 0u);
  }
  // The PC is not dereferenced even when it names the output itself.
  write(Output, Image);
  EXPECT_EQ(call("RtlPcToFileHeader", {Output, Output}), 0u);
  EXPECT_EQ(read(Output), 0u);
}

TEST_F(KernelObjects, PcLookupChecksOutputLifetimeRightsAndResidency) {
  constexpr uint64_t Image = 0x180000000;
  const auto Freed = call("ExAllocatePool2", {0x40, 8, 0x71756572});
  call("ExFreePool", {Freed});
  EXPECT_NE(fail("RtlPcToFileHeader", {Image, Freed}).find("freed"),
            std::string::npos);
  const auto Paged = call("ExAllocatePool2", {0x100, 8, 0x71756572});
  write(Paged, UINT64_MAX);
  call("KfRaiseIrql", {15});
  EXPECT_EQ(call("RtlPcToFileHeader", {Image, Scratch}), Image);
  EXPECT_NE(fail("RtlPcToFileHeader", {Image, Paged}).find("paged"),
            std::string::npos);
  EXPECT_EQ(read(Paged), UINT64_MAX);
  call("KeLowerIrql", {0});
  call("ExFreePool", {Paged});
  write(Scratch, UINT64_MAX);
  ok(Memory->protect(Scratch, 4096, Read));
  EXPECT_FALSE(fail("RtlPcToFileHeader", {Image, Scratch}).empty());
  EXPECT_EQ(read(Scratch), UINT64_MAX);
}

TEST_F(KernelObjects, PcLookupRequiresACompleteEightByteOutput) {
  const uint64_t Output = Scratch + 0x10000 - 4;
  write(Output, 0xa5a5a5a5, 4);
  EXPECT_FALSE(fail("RtlPcToFileHeader", {0, Output}).empty());
  EXPECT_EQ(read(Output, 4), 0xa5a5a5a5u);
}

TEST_F(KernelObjects, ReferencesBlockUnloadUntilReleased) {
  const auto Process = call("PsGetCurrentProcess", {});
  call("ObfReferenceObject", {Process});
  write(Model->driverObject() + 0x68, Entry);
  take(Model->beginUnload());
  auto E = Model->finishUnload();
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("object references"),
            std::string::npos);
  call("ObfDereferenceObject", {Process});
  ok(Model->finishUnload());
  EXPECT_TRUE(Result.UnloadCompleted);
}

TEST_F(KernelObjects, UnmodeledSecurityCannotGrantAccessOrConsumeHandles) {
  const auto Process = call("PsGetCurrentProcess", {});
  const auto Type = call("ObGetObjectType", {Process});
  write(Scratch, 0xabcdef);
  EXPECT_FALSE(
      fail("ObOpenObjectByPointer", {Process, 0, 0, 0, Type, 1, Scratch})
          .empty());
  EXPECT_FALSE(
      fail("ObOpenObjectByPointer", {Process, 0x400, 0, 0, Type, 0, Scratch})
          .empty());
  EXPECT_FALSE(fail("ObOpenObjectByPointer",
                    {Process, 0, Scratch + 32, 0, Type, 0, Scratch})
                   .empty());
  EXPECT_FALSE(fail("ObOpenObjectByPointer",
                    {Process, 0, 0, 0x10000000, Type, 0, Scratch})
                   .empty());
  Model->enterExecution(Scratch + 0x8000);
  EXPECT_FALSE(
      fail("ObOpenObjectByPointer", {Process, 0, 0, 0, Type, 0, Scratch})
          .empty());
  EXPECT_EQ(read(Scratch), 0xabcdefu);
  EXPECT_EQ(
      call("ObOpenObjectByPointer", {Process, 0x200, 0, 0, Type, 0, Scratch}),
      0u);
  EXPECT_EQ(read(Scratch), 0x60000000u);
  EXPECT_EQ(call("ZwClose", {read(Scratch)}), 0u);
  EXPECT_FALSE(fail("ObfDereferenceObject", {Process}).empty());
}
} // namespace
} // namespace neverd::emulation
