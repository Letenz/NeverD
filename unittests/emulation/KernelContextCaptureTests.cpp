//===- KernelContextCaptureTests.cpp - Kernel caller context contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent complete-byte, CPU-effect and pre-effect refusal controls.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelContextCapture.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <bit>

namespace neverd::emulation {
namespace {
using namespace llvm::support::endian;
constexpr uint64_t Base = 0x180000000, Data = 0x60000000;
constexpr uint64_t Stack = 0x71000000, Page = 4096, Limit = 16 * 1024 * 1024;
void check(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}
template <class T> T check(llvm::Expected<T> Value) {
  if (!Value) {
    ADD_FAILURE() << llvm::toString(Value.takeError());
    return {};
  }
  return std::move(*Value);
}
void denied(llvm::Expected<uint64_t> Value, llvm::StringRef Fragment = {}) {
  ASSERT_FALSE(bool(Value));
  auto Text = llvm::toString(Value.takeError());
  EXPECT_FALSE(Text.empty());
  EXPECT_NE(Text.find(Fragment.str()), std::string::npos) << Text;
}

// Subrange read refusal distinguishes the scratch read admission from the
// adjacent return read; ordinary page permissions cannot split these words.
struct Memory final : GuestMemory {
  std::shared_ptr<AddressSpace> Space =
      check(AddressSpace::create(check(PhysicalMemory::create(Limit)), Limit));
  uint64_t DenyRead = 0, FailRead = 0;
  unsigned Writes = 0;
  std::shared_ptr<AddressSpace> addressSpace() const override { return Space; }
  llvm::Expected<MemoryView> pinBacking(uint64_t A, uint64_t N) const override {
    return Space->pinBacking(A, N);
  }
  llvm::Error validatePinned(const MemoryView &V, uint64_t O,
                             uint64_t N) const override {
    return Space->validatePinned(V, O, N);
  }
  llvm::Error readPinned(const MemoryView &V, uint64_t O,
                         llvm::MutableArrayRef<uint8_t> B) override {
    return Space->readPinned(V, O, B);
  }
  llvm::Error writePinned(const MemoryView &V, uint64_t O,
                          llvm::ArrayRef<uint8_t> B) override {
    ++Writes;
    return Space->writePinned(V, O, B);
  }
  llvm::Error readBacking(uint64_t A,
                          llvm::MutableArrayRef<uint8_t> B) override {
    return Space->readBacking(A, B);
  }
  llvm::Error writeBacking(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    ++Writes;
    return Space->writeBacking(A, B);
  }
  llvm::Error map(uint64_t A, uint64_t N, unsigned P) override {
    return Space->map(A, N, P);
  }
  llvm::Error protect(uint64_t A, uint64_t N, unsigned P) override {
    return Space->protect(A, N, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (A == FailRead)
      return llvm::createStringError("injected return read failure");
    return Space->read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    ++Writes;
    return Space->write(A, B);
  }
  llvm::Error validateBacking(uint64_t A, uint64_t N) const override {
    return Space->validateBacking(A, N);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t N,
                                 unsigned P) const override {
    if (A == DenyRead && (P & Read))
      return false;
    return Space->canAccess(A, N, P);
  }
  llvm::Error snapshotBacking(uint64_t A,
                              llvm::MutableArrayRef<uint8_t> B) override {
    return Space->snapshotBacking(A, B);
  }
};

struct Session {
  Memory RAM;
  DriverResult Result;
  KernelModel Model{RAM, Result};
  KernelCapturedContext State;
  uint64_t Destination = Data + 32, SP = Stack + 2056, Flags = 0x202;
  unsigned Reads = 0, Sets = 0;
  bool FailCollect = false, FailSet = false;
  explicit Session(ExecutionContract Contract = ExecutionContract::CheckedX64) {
    check(RAM.map(Base, 2 * Page, Read | Write));
    check(RAM.protect(Base, Page, Read | Execute));
    check(RAM.map(Data, 3 * Page, Read | Write));
    check(RAM.map(Stack, 2 * Page, Read | Write));
    DriverImage Image;
    Image.Base = Base;
    Image.Size = 2 * Page;
    Image.Entry = Base;
    Image.Regions = {{Base, Read | Execute, std::vector<uint8_t>(Page)},
                     {Base + Page, Read | Write, std::vector<uint8_t>(Page)}};
    DriverOptions Options;
    Options.Contract = Contract;
    check(Model.initialize(Image, Options));
    prepare();
  }
  void prepare(uint8_t Fill = 0xa5, uint64_t IncomingFlags = 0x202) {
    Flags = IncomingFlags;
    check(RAM.Space->write(Data, std::vector<uint8_t>(3 * Page, Fill)));
    check(RAM.Space->write(Stack, std::vector<uint8_t>(2 * Page, 0xd3)));
    check(RAM.Space->writeInteger(SP, Base + 128, 8));
    State.Bytes.fill(0x87);
    for (unsigned I = 0; I < 16; ++I)
      write64le(State.Bytes.data() + 120 + 8 * I, 0x1234000000000000ULL + I);
    write64le(State.Bytes.data() + 128, Destination);
    write64le(State.Bytes.data() + 152, SP);
    write64le(State.Bytes.data() + 248, 0xdeadbeef); // Provider PC is not RIP.
    write32le(State.Bytes.data() + 52, 0x3fa1);
    for (unsigned I = 0; I < 6; ++I)
      write16le(State.Bytes.data() + 56 + I * 2, I ? 16 : 8);
    write32le(State.Bytes.data() + 68, Flags);
    write16le(State.Bytes.data() + 256, 0x27f);
    write32le(State.Bytes.data() + 280, 0x3fa1);
    for (unsigned I = 0; I < 16; ++I) {
      write64le(State.Bytes.data() + 416 + I * 16, 0x1122334455667700ULL + I);
      write64le(State.Bytes.data() + 424 + I * 16, 0x8877665544332200ULL + I);
    }
    RAM.Writes = Reads = Sets = 0;
  }
  std::vector<uint8_t> bytes(uint64_t A, unsigned N) {
    std::vector<uint8_t> B(N);
    check(RAM.Space->snapshotBacking(A, B));
    return B;
  }
  llvm::Expected<uint64_t> capture() {
    auto Read = [&]() -> llvm::Expected<KernelCapturedContext> {
      ++Reads;
      if (FailCollect)
        return llvm::createStringError("injected collector failure");
      return State;
    };
    auto Set = [&](uint64_t Value) -> llvm::Error {
      ++Sets;
      if (FailSet)
        return llvm::createStringError("injected setter failure");
      Flags = Value;
      return llvm::Error::success();
    };
    KernelContextCapture Context{Read, Set};
    return Model.call("RtlCaptureContext", {Destination}, nullptr, &Context);
  }
  void reject(llvm::StringRef Fragment = {}) {
    const auto DataBefore = bytes(Data, 3 * Page);
    const auto StackBefore = bytes(Stack, 2 * Page);
    const auto FlagsBefore = Flags;
    RAM.Writes = 0;
    denied(capture(), Fragment);
    EXPECT_EQ(bytes(Data, 3 * Page), DataBefore);
    EXPECT_EQ(bytes(Stack, 2 * Page), StackBefore);
    EXPECT_EQ(Flags, FlagsBefore);
    EXPECT_EQ(RAM.Writes, 0u);
  }
};

TEST(KernelContextCapture, ExactRecordScratchAndFlagsPreserveEveryOtherByte) {
  Session S;
  for (uint8_t Fill : {0x5a, 0xa5})
    for (uint64_t Flags : {0x202u, 0xed7u, 0x402u, 2u})
      for (unsigned Offset : {8u, 24u, 40u, 248u}) {
        S.SP = Stack + 1024 + Offset;
        S.prepare(Fill, Flags);
        auto Expected = S.bytes(Data, 3 * Page);
        auto *P = Expected.data() + 32;
        write32le(P + 48, 0x10000f);
        write32le(P + 52, 0x3fa1);
        for (unsigned I = 0; I < 6; ++I)
          write16le(P + 56 + 2 * I, I ? 16 : 8);
        write32le(P + 68, Flags);
        for (unsigned I = 0; I < 16; ++I)
          write64le(P + 120 + I * 8, I == 1   ? S.Destination
                                     : I == 4 ? S.SP + 8
                                              : 0x1234000000000000ULL + I);
        write64le(P + 248, Base + 128);
        write16le(P + 256, 0x27f);
        write32le(P + 258, 0);
        write32le(P + 280, 0x3fa1);
        for (unsigned I = 0; I < 16; ++I) {
          write64le(P + 416 + I * 16, 0x1122334455667700ULL + I);
          write64le(P + 424 + I * 16, 0x8877665544332200ULL + I);
        }
        auto ExpectedStack = S.bytes(Stack, 2 * Page);
        write64le(ExpectedStack.data() + S.SP - Stack - 8, Flags);
        const auto OriginalState = S.State.Bytes;
        EXPECT_EQ(check(S.capture()), Flags);
        EXPECT_EQ(S.bytes(Data, 3 * Page), Expected);
        EXPECT_EQ(S.bytes(Stack, 2 * Page), ExpectedStack);
        // All admitted SP values are aligned 8 modulo 16, so the final ADD
        // cannot carry, overflow, reach zero, or set AF. Parity still varies.
        EXPECT_EQ(S.Flags, (Flags & ~0x8d5ULL) |
                               (!(std::popcount(uint8_t(S.SP)) & 1) ? 4 : 0));
        EXPECT_EQ(S.State.Bytes, OriginalState);
        EXPECT_EQ(S.Reads, 1u);
        EXPECT_EQ(S.Sets, 1u);
      }
}

TEST(KernelContextCapture, HighStackAndCrossPageRecordUseActualReturnWord) {
  Session S;
  constexpr uint64_t High = 0xffff800000010000ULL;
  check(S.RAM.map(High, Page, Read | Write));
  S.Destination = Data + Page - 16;
  S.SP = High + 24;
  S.prepare();
  check(S.RAM.Space->writeInteger(S.SP, Base + 256, 8));
  EXPECT_EQ(check(S.capture()), 0x202u);
  const auto B = S.bytes(S.Destination, 1232);
  EXPECT_EQ(read64le(B.data() + 248), Base + 256);
  EXPECT_EQ(read64le(B.data() + 152), S.SP + 8);
  EXPECT_EQ(S.Flags, 0x286u);
}

TEST(KernelContextCapture, MissingBridgeLegacyAndArityFailWithoutCollection) {
  Session S;
  denied(S.Model.call("RtlCaptureContext", {S.Destination}), "stopped CPU");
  denied(S.Model.call("RtlCaptureContext", {}), "argument count");
  EXPECT_EQ(S.Reads, 0u);
  EXPECT_EQ(S.RAM.Writes, 0u);
  Session Legacy(ExecutionContract::Legacy);
  Legacy.reject("checked-x64");
  EXPECT_EQ(Legacy.Reads, 0u);
}

TEST(KernelContextCapture, CollectionSetterAndReturnReadFailuresHaveNoEffects) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    Session S;
    S.FailCollect = Mode == 0;
    S.FailSet = Mode == 1;
    S.RAM.FailRead = Mode == 2 ? S.SP : 0;
    S.reject("injected");
    EXPECT_EQ(S.Sets, Mode == 1 ? 1u : 0u);
  }
}

TEST(KernelContextCapture, RejectsMalformedAndMismatchedCallerState) {
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    Session S;
    if (Mode < 3)
      write64le(S.State.Bytes.data() + 152, Mode == 0   ? 0
                                            : Mode == 1 ? UINT64_MAX
                                                        : S.SP + 1);
    else if (Mode == 3)
      write64le(S.State.Bytes.data() + 128, S.Destination + 16);
    else
      write32le(S.State.Bytes.data() + 68, 0x10000);
    S.reject("caller CPU");
    EXPECT_EQ(S.Sets, 0u);
  }
}

TEST(KernelContextCapture, RejectsUnalignedOverflowAndUserDestinations) {
  for (uint64_t A :
       {uint64_t(0), Data + 1, UINT64_MAX - 15, uint64_t(0x20000)}) {
    Session S;
    check(S.RAM.map(0x20000, Page, Read | Write | UserAccessible));
    S.Destination = A;
    write64le(S.State.Bytes.data() + 128, A);
    S.reject();
    EXPECT_EQ(S.Sets, 0u);
  }
}

TEST(KernelContextCapture, CompleteRecordReadWriteRightsPrecedeAllEffects) {
  for (unsigned Rights : {unsigned(Read), unsigned(Write), 0u}) {
    Session S;
    S.Destination = Data + Page - 16;
    S.prepare();
    check(S.RAM.protect(Data + Page, Page, Rights));
    S.reject();
    EXPECT_EQ(S.Sets, 0u);
  }
  Session Partial;
  Partial.Destination = Data + 3 * Page - 16;
  Partial.prepare();
  Partial.reject(); // The record extends beyond the last mapped page.
  EXPECT_EQ(Partial.Sets, 0u);
}

TEST(KernelContextCapture, ScratchNeedsReadAsWellAsWritePermission) {
  Session S;
  S.RAM.DenyRead = S.SP - 8;
  S.reject("accessible ordinary RAM");
  EXPECT_EQ(S.Sets, 0u);
  S.RAM.DenyRead = 0;
  check(S.RAM.protect(Stack, 2 * Page, Read));
  S.reject();
  EXPECT_EQ(S.Sets, 0u);
}

TEST(KernelContextCapture, RejectsReturnAndScratchOverlapBeforeMutation) {
  for (int64_t Displacement : {-8, -24, -1224}) {
    Session S;
    S.Destination = S.SP + Displacement;
    write64le(S.State.Bytes.data() + 128, S.Destination);
    S.reject("overlap");
    EXPECT_EQ(S.Sets, 0u);
  }
}

TEST(KernelContextCapture, RejectsPhysicalAliasesEvenWithNoAccess) {
  for (uint64_t Source : {Data, Stack})
    for (unsigned Rights : {0u, unsigned(Read | Write)}) {
      Session S;
      check(S.RAM.Space->mapAlias(Data + 4 * Page, Source, Page, Rights));
      S.reject("aliased");
      EXPECT_EQ(S.Sets, 0u);
    }
}

TEST(KernelContextCapture, DeviceFreedAndOpaqueStorageRemainIneligible) {
  Session S;
  unsigned DeviceCalls = 0;
  GuestMMIOCallbacks Callbacks;
  Callbacks.Validate = [&](uint64_t, uint64_t, bool) {
    ++DeviceCalls;
    return llvm::Error::success();
  };
  Callbacks.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++DeviceCalls;
    return 0;
  };
  Callbacks.Write = [&](uint64_t, unsigned, uint64_t) {
    ++DeviceCalls;
    return llvm::Error::success();
  };
  const uint64_t Device = Data + 4 * Page;
  check(S.RAM.Space->mapMMIO(Device, Page, std::move(Callbacks)));
  const auto Pool = check(S.Model.call("ExAllocatePool", {0, 2048}));
  check(S.Model.call("ExFreePool", {Pool}));
  for (uint64_t Address : {Device, Pool, S.Model.driverObject()}) {
    S.Destination = Address;
    write64le(S.State.Bytes.data() + 128, Address);
    S.reject();
  }
  EXPECT_EQ(DeviceCalls, 0u);
  EXPECT_EQ(S.Sets, 0u);
}

TEST(KernelContextCapture, DispatchIRQLAllowedHigherIRQLRejected) {
  Session S;
  S.Model.enterExecution(Stack);
  check(S.Model.call("KfRaiseIrql", {2}));
  EXPECT_EQ(check(S.capture()), 0x202u);
  check(S.Model.call("KfRaiseIrql", {3}));
  S.prepare();
  S.reject("IRQL");
  EXPECT_EQ(S.Reads, 0u);
  check(S.Model.call("KeLowerIrql", {2}));
  check(S.Model.call("KeLowerIrql", {0}));
  check(S.Model.validateExecutionReturn(Stack, 0));
}

TEST(KernelContextCapture, OverwritingCaptureDoesNotRemoveRecoveryDependency) {
  Session S;
  check(S.Model.captureUnpackBaseline());
  const auto Before = check(S.Model.unpackDependencies());
  EXPECT_EQ(std::count(Before.begin(), Before.end(),
                       "unreconstructed kernel or environment effects"),
            0);
  EXPECT_EQ(check(S.capture()), 0x202u);
  S.prepare(0); // No retained numerical observation authorizes reconstruction.
  const auto After = check(S.Model.unpackDependencies());
  EXPECT_EQ(std::count(After.begin(), After.end(),
                       "unreconstructed kernel or environment effects"),
            1);
}
} // namespace
} // namespace neverd::emulation
