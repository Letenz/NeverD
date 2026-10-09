//===- ProcessTransferTests.cpp - Exact instruction observation reuse ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "unpack/dynamic/ProcessTransfer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <map>

namespace neverd::unpack {
namespace {
using namespace emulation;

class TransferImage final : public InputImage {
public:
  TransferImage() : InputImage(FormatKind::PE64, {}) { Extent = 8192; }
};

class TransferProcess final : public ProcessView {
public:
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(8192);
  std::map<uint64_t, uint32_t> Sizes;
  uint64_t SP = 0x8000;
  bool MemoryUnchanged = false;
  bool InputPresent = true, EntryInvocation = true;
  bool FailTLS = false;
  unsigned Reads = 0;
  std::vector<uint64_t> Initializers;
  std::optional<std::vector<uint8_t>> TLS;
  std::optional<ProcessCallFrame> Frame;
  std::optional<std::vector<ProcessHeapAllocationView>> Heap =
      std::vector<ProcessHeapAllocationView>{};
  GuestArchitecture architecture() const override {
    return GuestArchitecture::X64;
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    EXPECT_EQ(R, CPURegister::X64SP);
    return RegisterValue{SP, 0};
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> Out) override {
    ++Reads;
    if (A > Bytes.size() || Out.size() > Bytes.size() - A)
      return llvm::createStringError("test read outside the image");
    llvm::copy(llvm::ArrayRef(Bytes).slice(A, Out.size()), Out.begin());
    return llvm::Error::success();
  }
  llvm::Expected<std::vector<AddressMapping>> mappings() override {
    return std::vector<AddressMapping>{
        {0, Bytes.size(), Read | Write | Execute, false}};
  }
  std::vector<ProcessModuleView> modules() override {
    return {{"independent.exe", 0, Bytes.size(), 0, true, false}};
  }
  std::optional<ProcessModuleView> inputModule() override {
    return InputPresent ? ProcessView::inputModule() : std::nullopt;
  }
  std::vector<ProcessExportView> exports() override { return {}; }
  bool programInvocation() const override { return EntryInvocation; }
  std::optional<std::vector<ProcessHeapAllocationView>>
  heapAllocations() const override {
    return Heap;
  }
  std::vector<uint64_t> completedInitializers() const override {
    return Initializers;
  }
  llvm::Expected<std::optional<std::vector<uint8_t>>>
  threadLocalMemory() override {
    if (FailTLS)
      return llvm::createStringError("test TLS snapshot failed");
    return TLS;
  }
  llvm::Expected<std::optional<ProcessCallFrame>> callFrame() override {
    return Frame;
  }
  bool watchedMemoryUnchanged() const override { return MemoryUnchanged; }
  llvm::Expected<uint32_t> instructionSize(uint64_t A) override {
    auto I = Sizes.find(A);
    if (I == Sizes.end())
      return llvm::createStringError("test has no stopped decoder result");
    return I->second;
  }
  void instruction(uint64_t A, llvm::ArrayRef<uint8_t> Code) {
    llvm::copy(Code, Bytes.begin() + A);
    Sizes.emplace(A, Code.size());
  }
};

bool watched(llvm::ArrayRef<ExecutionWatch> Watches, uint64_t PC) {
  return llvm::any_of(Watches, [&](const auto &W) {
    return PC >= W.Address && PC - W.Address < W.Size;
  });
}

class ProcessTransfer : public testing::Test {
protected:
  TransferImage Image;
  TransferProcess Process;
  TransferObserver Observer{Image, {CPURegister::X64SP, 15}, 0};
  void start() { llvm::cantFail(Observer.started(Process)); }
  std::vector<ExecutionWatch> observe(uint64_t PC) {
    auto Next = llvm::cantFail(Observer.watched(Process, PC));
    EXPECT_TRUE(Next.has_value());
    return Next.value_or(std::vector<ExecutionWatch>{});
  }
  std::vector<ExecutionWatch> refresh() {
    auto Next = llvm::cantFail(Observer.resuming(Process));
    EXPECT_TRUE(Next.has_value());
    return Next.value_or(std::vector<ExecutionWatch>{});
  }
};

TEST_F(ProcessTransfer, CapturesUnalignedAndInteriorHeapReferences) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  Process.Bytes[65] = 42;
  Process.Heap =
      std::vector<ProcessHeapAllocationView>{{0x12000, 0x100}, {0x10000, 0x80}};
  using llvm::support::endian::write64le;
  write64le(Process.Bytes.data() + 101, 0x10017);
  write64le(Process.Bytes.data() + 200, 0x10080); // Exclusive end.
  write64le(Process.Bytes.data() + 8192 - 8, 0x12000);
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  const auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  const auto &State = Captured->RuntimeState;
  EXPECT_TRUE(State.HeapInventoryKnown);
  EXPECT_EQ(State.PossibleHeapReferences, 2u);
  ASSERT_EQ(State.HeapReferences.size(), 2u);
  EXPECT_EQ(State.HeapReferences[0].Offset, 101u);
  EXPECT_EQ(State.HeapReferences[0].Location,
            UnpackHeapReference::Storage::Image);
  EXPECT_EQ(State.HeapReferences[0].Address, 0x10017u);
  EXPECT_EQ(State.HeapReferences[0].AllocationAddress, 0x10000u);
  EXPECT_EQ(State.HeapReferences[0].AllocationSize, 0x80u);
  EXPECT_EQ(State.HeapReferences[1].Offset, 8184u);
}

TEST_F(ProcessTransfer, MissingHeapInventoryRemainsUnknown) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  Process.Heap.reset();
  Process.Bytes[65] = 42;
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  const auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  EXPECT_FALSE(Captured->RuntimeState.HeapInventoryKnown);
}

TEST_F(ProcessTransfer, HeapReferencesInCapturedTLSCannotBeDiscarded) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  Process.Bytes[65] = 42;
  Process.Heap = std::vector<ProcessHeapAllocationView>{{0x10000, 0x80}};
  Process.TLS = std::vector<uint8_t>(17);
  llvm::support::endian::write64le(Process.TLS->data() + 3, 0x10017);
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  const auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  EXPECT_TRUE(Captured->RuntimeState.HeapInventoryKnown);
  EXPECT_EQ(Captured->RuntimeState.PossibleHeapReferences, 1u);
  ASSERT_EQ(Captured->RuntimeState.HeapReferences.size(), 1u);
  const auto &Reference = Captured->RuntimeState.HeapReferences.front();
  EXPECT_EQ(Reference.Location, UnpackHeapReference::Storage::ThreadLocal);
  EXPECT_EQ(Reference.Offset, 3u);
  EXPECT_EQ(Reference.Address, 0x10017u);
}

TEST_F(ProcessTransfer,
       HeapReferenceReportingRemainsBoundedWithoutLosingCount) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  Process.Bytes[65] = 42;
  Process.Heap = std::vector<ProcessHeapAllocationView>{{0x10000, 0x80}};
  for (unsigned I = 0; I != 80; ++I)
    llvm::support::endian::write64le(Process.Bytes.data() + 1024 + I * 8,
                                     0x10017);
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  const auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  EXPECT_EQ(Captured->RuntimeState.PossibleHeapReferences, 80u);
  ASSERT_EQ(Captured->RuntimeState.HeapReferences.size(), 64u);
  EXPECT_EQ(Captured->RuntimeState.HeapReferences.front().Offset, 1024u);
  EXPECT_EQ(Captured->RuntimeState.HeapReferences.back().Offset, 1528u);
}

TEST_F(ProcessTransfer, InvalidHeapInventoryCannotAuthorizeCapture) {
  for (const std::vector<ProcessHeapAllocationView> Heap :
       {std::vector<ProcessHeapAllocationView>{{0x10000, 0}},
        {{0x10000, 0x100}, {0x10080, 0x100}},
        {{UINT64_MAX, 2}}}) {
    TransferProcess P;
    TransferObserver O{Image, {CPURegister::X64SP, 15}, 0};
    P.instruction(64, {0xb8, 0, 0, 0, 0});
    llvm::cantFail(O.started(P));
    P.Heap = Heap;
    P.Bytes[65] = 42;
    auto Stopped = O.watched(P, 64);
    ASSERT_FALSE(bool(Stopped));
    EXPECT_NE(llvm::toString(Stopped.takeError()).find("heap inventory"),
              std::string::npos);
    EXPECT_FALSE(O.take());
  }
}

TEST_F(ProcessTransfer, ReusesWholeInstructionsWithUnchangedOpcodeBytes) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  Process.instruction(128, {0x83, 0xe9, 0});
  start();
  Process.SP -= 40;
  Process.Bytes[65] = 1;
  Process.Bytes[130] = 1;
  auto Watches = observe(64);
  ASSERT_EQ(Observer.transfers().size(), 1u);
  EXPECT_FALSE(watched(Watches, 64));
  Watches = observe(128);
  EXPECT_FALSE(watched(Watches, 128));
  // An alternative entry inside the instruction has no observation proof.
  EXPECT_TRUE(watched(Watches, 129));
  EXPECT_FALSE(llvm::cantFail(Observer.resuming(Process)).has_value());
  Process.Bytes[1000] = 42;
  Watches = refresh();
  EXPECT_FALSE(watched(Watches, 64));
  EXPECT_FALSE(watched(Watches, 128));
  Process.Bytes[130] = 2;
  Watches = refresh();
  EXPECT_TRUE(watched(Watches, 128));
  EXPECT_FALSE(watched(Watches, 64));
  observe(128);
  ASSERT_EQ(Observer.transfers().size(), 2u);
  EXPECT_EQ(Observer.transfers().back().Generation, 2u);
}

TEST_F(ProcessTransfer, ADelayedInputUsesItsOwnInvocationStackAndBaseline) {
  Process.InputPresent = false;
  Process.EntryInvocation = false;
  EXPECT_TRUE(llvm::cantFail(Observer.started(Process)).empty());
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  Process.InputPresent = true;
  auto Initial = llvm::cantFail(Observer.invoking(Process));
  ASSERT_TRUE(Initial);
  EXPECT_TRUE(watched(*Initial, 64));
  Process.SP -= 64;
  Process.EntryInvocation = true;
  llvm::cantFail(Observer.invoking(Process));
  Process.Bytes[65] = 42;
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  EXPECT_EQ(Captured->EntryRVA, 64u);
  EXPECT_EQ(Captured->Baseline[65], 0);
  EXPECT_EQ(Captured->Memory[65], 42);
  ASSERT_EQ(Observer.transfers().size(), 1u);
  EXPECT_TRUE(Observer.transfers().front().StackBalanced);
  EXPECT_TRUE(Observer.transfers().front().ProgramInvocation);
}

TEST_F(ProcessTransfer, UnchangedVisitedPagesKeepOnlyUnvisitedPageWatches) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  auto Watches = observe(64);
  ASSERT_EQ(Watches.size(), 1u);
  EXPECT_EQ(Watches.front().Address, 4096u);
  EXPECT_EQ(Watches.front().Size, 4096u);
  Process.instruction(4096, {0xb8, 0, 0, 0, 0});
  // Materializing the previously unvisited page is still a new generation.
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 4096)));
  ASSERT_TRUE(Observer.take());
}

TEST_F(ProcessTransfer, AnInvocationRetainsChangesFromEverySavedImage) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  Process.SP -= 64;
  Process.Bytes[65] = 42;
  observe(64);
  ASSERT_EQ(Observer.transfers().size(), 1u);
  Process.SP = 0x8000;
  auto Watches = llvm::cantFail(Observer.invoking(Process));
  ASSERT_TRUE(Watches);
  EXPECT_TRUE(watched(*Watches, 64));
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  EXPECT_EQ(Captured->Baseline[65], 0);
  EXPECT_EQ(Captured->Memory[65], 42);
  ASSERT_EQ(Captured->Transfers.size(), 2u);
  EXPECT_TRUE(Captured->Transfers.back().StackBalanced);
}

TEST_F(ProcessTransfer, GeneratedCallsNeedTheirReturnedStackAtTheContinuation) {
  for (bool Returns : {false, true}) {
    SCOPED_TRACE(Returns);
    TransferProcess P;
    TransferObserver O{Image, {CPURegister::X64SP, 15}, 0};
    P.instruction(64, {0xb8, 0, 0, 0, 0});
    P.instruction(256, {0xb8, 0, 0, 0, 0});
    llvm::cantFail(O.started(P));
    P.SP -= 64;
    P.Frame = ProcessCallFrame{0x20000, 0x8000, {0, 1, 0}};
    P.Bytes[65] = 1;
    auto Entry = llvm::cantFail(O.watched(P, 64));
    ASSERT_TRUE(Entry);
    EXPECT_TRUE(watched(*Entry, 0x20000));
    if (Returns)
      P.SP = 0x8000;
    auto Returned = llvm::cantFail(O.watched(P, 0x20000));
    ASSERT_TRUE(Returned);
    P.SP = 0x8000;
    P.Frame.reset();
    P.Bytes[257] = 42;
    EXPECT_FALSE(llvm::cantFail(O.watched(P, 256)));
    auto Captured = O.take();
    ASSERT_TRUE(Captured);
    EXPECT_EQ(Captured->CompletedCalls.size(), Returns ? 1u : 0u);
    if (Returns) {
      EXPECT_EQ(Captured->CompletedCalls.front().Entry, 64u);
      EXPECT_EQ(Captured->CompletedCalls.front().Arguments[1], 1u);
    }
  }
}

TEST_F(ProcessTransfer, SelectedTransfersCaptureInitializerAndThreadState) {
  for (bool ProgramInvocation : {false, true}) {
    SCOPED_TRACE(ProgramInvocation);
    TransferProcess P;
    TransferObserver O{Image, {CPURegister::X64SP, 15}, 1};
    P.EntryInvocation = ProgramInvocation;
    P.instruction(64, {0xb8, 0, 0, 0, 0});
    llvm::cantFail(O.started(P));
    P.Initializers = {512};
    P.TLS = std::vector<uint8_t>{0x42, 0, 0x7b};
    P.Bytes[65] = 1;
    EXPECT_FALSE(llvm::cantFail(O.watched(P, 64)));
    auto Captured = O.take();
    ASSERT_TRUE(Captured);
    EXPECT_EQ(Captured->Initializers, P.Initializers);
    EXPECT_EQ(Captured->ThreadLocal, P.TLS);
    ASSERT_EQ(Captured->Transfers.size(), 1u);
    EXPECT_EQ(Captured->Transfers.front().ProgramInvocation, ProgramInvocation);
  }
}

TEST_F(ProcessTransfer, SelectedTransfersRejectFailedThreadStateCapture) {
  for (bool ProgramInvocation : {false, true}) {
    SCOPED_TRACE(ProgramInvocation);
    TransferProcess P;
    TransferObserver O{Image, {CPURegister::X64SP, 15}, 1};
    P.EntryInvocation = ProgramInvocation;
    P.instruction(64, {0xb8, 0, 0, 0, 0});
    llvm::cantFail(O.started(P));
    P.Bytes[65] = 1;
    P.FailTLS = true;
    auto Result = O.watched(P, 64);
    ASSERT_FALSE(bool(Result));
    EXPECT_EQ(llvm::toString(Result.takeError()), "test TLS snapshot failed");
    EXPECT_FALSE(O.take());
  }
}

TEST_F(ProcessTransfer, CrossPageOperandWritesRearmAnObservedInstruction) {
  Process.instruction(4094, {0xb8, 0, 0, 0, 0});
  start();
  Process.SP -= 40;
  Process.Bytes[4096] = 1;
  EXPECT_FALSE(watched(observe(4094), 4094));
  const auto Writes = Observer.writeWatches();
  EXPECT_TRUE(llvm::any_of(Writes, [](const auto &W) {
    return W.Address <= 4097 && 4097 - W.Address < W.Size;
  }));
  Process.Bytes[4097] = 2;
  EXPECT_TRUE(watched(refresh(), 4094));
  observe(4094);
  ASSERT_EQ(Observer.transfers().size(), 2u);
  EXPECT_EQ(Observer.transfers().back().Generation, 2u);
}

TEST_F(ProcessTransfer, OpcodeChangesWithdrawThePreviousDecodedExtent) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  start();
  Process.SP -= 40;
  Process.Bytes[65] = 1;
  EXPECT_FALSE(watched(observe(64), 64));
  Process.Bytes[64] = 0xeb;
  Process.Sizes[64] = 2;
  EXPECT_TRUE(watched(refresh(), 64));
  EXPECT_FALSE(watched(observe(64), 64));
  ASSERT_EQ(Observer.transfers().size(), 2u);
  Process.Bytes[68] = 0x90;
  EXPECT_FALSE(watched(refresh(), 64));
}

TEST_F(ProcessTransfer, ReturningToOlderCodeWithdrawsInstructionEvidence) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  Process.instruction(128, {0x83, 0xe9, 0});
  Process.instruction(512, {0xc3});
  start();
  Process.SP -= 40;
  Process.Bytes[65] = 1;
  Process.Bytes[130] = 1;
  observe(64);
  EXPECT_FALSE(watched(observe(128), 128));
  auto Watches = observe(512);
  EXPECT_TRUE(watched(Watches, 64));
  EXPECT_TRUE(watched(Watches, 128));
  EXPECT_EQ(Observer.transfers().size(), 1u);
}

TEST_F(ProcessTransfer, CleanWatchResumptionReusesCurrentSnapshots) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  Process.instruction(128, {0x83, 0xe9, 0});
  start();
  observe(64);
  llvm::cantFail(Observer.resuming(Process));
  observe(128);
  Process.MemoryUnchanged = true;
  Process.Reads = 0;
  EXPECT_FALSE(llvm::cantFail(Observer.resuming(Process)).has_value());
  EXPECT_EQ(Process.Reads, 0u);
  Process.MemoryUnchanged = false;
  Process.Bytes[130] = 1;
  EXPECT_TRUE(watched(refresh(), 128));
}

TEST_F(ProcessTransfer, FirstVisitRefreshesPreviouslyUnwatchedFetchTails) {
  Process.instruction(64, {0x90});
  Process.instruction(4094, {0xb8, 0, 0, 0, 0});
  start();
  // No installed write watch covered this tail before the first page visit.
  Process.Bytes[4096] = 1;
  auto Watches = observe(64);
  EXPECT_FALSE(watched(Watches, 4094));
  Process.MemoryUnchanged = true;
  EXPECT_TRUE(watched(refresh(), 4094));
}

TEST_F(ProcessTransfer, RestoredPageBytesRetainTheirNewerGeneration) {
  Process.instruction(64, {0xb8, 0, 0, 0, 0});
  Process.instruction(4096, {0x90});
  start();
  observe(64);
  observe(4096);
  Process.SP -= 40;
  Process.Bytes[65] = 1;
  EXPECT_TRUE(watched(refresh(), 64));
  observe(64);
  EXPECT_TRUE(watched(observe(4096), 64));
  // Reverting to the original file bytes is a change from the later image.
  Process.Bytes[65] = 0;
  EXPECT_TRUE(watched(refresh(), 64));
  observe(64);
  ASSERT_EQ(Observer.transfers().size(), 2u);
  EXPECT_EQ(Observer.transfers().back().Generation, 2u);
  EXPECT_TRUE(watched(observe(4096), 64));
  Process.SP = 0x8000;
  EXPECT_FALSE(llvm::cantFail(Observer.watched(Process, 64)));
  auto Captured = Observer.take();
  ASSERT_TRUE(Captured);
  EXPECT_EQ(Captured->Baseline[65], 0);
  EXPECT_EQ(Captured->Memory[65], 0);
  EXPECT_EQ(Captured->Transfers.back().Generation, 2u);
}
} // namespace
} // namespace neverd::unpack
