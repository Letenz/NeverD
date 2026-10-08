//===- ProcessTransferTests.cpp - Exact instruction observation reuse ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "unpack/dynamic/ProcessTransfer.h"

#include "llvm/ADT/STLExtras.h"
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
  unsigned Reads = 0;
  std::optional<ProcessCallFrame> Frame;
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
} // namespace
} // namespace neverd::unpack
