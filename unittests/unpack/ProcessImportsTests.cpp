//===- ProcessImportsTests.cpp - Nested import observation stops ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "unpack/dynamic/ProcessImports.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

#include <map>

namespace neverd::unpack {
namespace {
using namespace emulation;

class ImportProcess final : public ProcessView {
public:
  static constexpr uint64_t Inner = 64, Outer = 128, Gate = 0x3000;
  static constexpr uint64_t InitialSP = 0x1f00;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(8192);
  std::map<CPURegister, RegisterValue> Registers;
  uint64_t NativeCalls = 0;

  ImportProcess() {
    for (uint64_t Start : {Inner, Outer}) {
      Bytes[Start] = 0xe8;
      llvm::support::endian::write32le(Bytes.data() + Start + 1,
                                       384 - (Start + 5));
      Bytes[Start + 5] = 0xee;
    }
    stop(Outer, InitialSP);
  }
  GuestArchitecture architecture() const override {
    return GuestArchitecture::X64;
  }
  llvm::Expected<RegisterValue> readRegister(CPURegister R) override {
    return Registers[R];
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Out) override {
    if (Address > Bytes.size() || Out.size() > Bytes.size() - Address)
      return llvm::createStringError("test read outside RAM");
    llvm::copy(llvm::ArrayRef(Bytes).slice(Address, Out.size()), Out.begin());
    return llvm::Error::success();
  }
  llvm::Expected<std::vector<AddressMapping>> mappings() override {
    return std::vector<AddressMapping>{{0, 4096, Read | Write | Execute, false},
                                       {4096, 4096, Read | Write, false}};
  }
  std::vector<ProcessModuleView> modules() override {
    return {{"independent.exe", 0, 4096, Outer, true, false}};
  }
  std::vector<ProcessExportView> exports() override {
    return {{Gate, "kernel32.dll", "GetCurrentProcessId", std::nullopt}};
  }
  std::optional<ProcessStackView> stack() const override {
    return ProcessStackView{4096, 4096};
  }
  std::optional<uint64_t> nativeCallCount() const override {
    return NativeCalls;
  }
  void stop(uint64_t PC, uint64_t SP) {
    Registers[CPURegister::X64PC] = {PC, 0};
    Registers[CPURegister::X64SP] = {SP, 0};
  }
  void exported(uint64_t Start) {
    stop(Gate, InitialSP - 8);
    llvm::support::endian::write64le(Bytes.data() + InitialSP - 8, Start + 6);
  }
};

class ProcessImports : public testing::Test {
protected:
  ImportProcess Process;
  const std::array<uint64_t, 2> Continuations = {ImportProcess::Inner + 6,
                                                 ImportProcess::Outer + 6};
  const std::array<uint64_t, 1> Gates = {ImportProcess::Gate};
  ImportObserver Observer{Continuations, Gates};
  void start() { llvm::cantFail(Observer.started(Process)); }
  void observe(uint64_t PC, uint64_t SP = ImportProcess::InitialSP) {
    Process.stop(PC, SP);
    ASSERT_TRUE(llvm::cantFail(Observer.watched(Process, PC)));
  }
  void exported(uint64_t Start) {
    Process.exported(Start);
    ASSERT_TRUE(llvm::cantFail(Observer.watched(Process, ImportProcess::Gate)));
  }
};

TEST_F(ProcessImports, NestedStopsPreserveTheOuterCallSnapshot) {
  start();
  observe(ImportProcess::Outer);
  observe(ImportProcess::Inner, ImportProcess::InitialSP - 24);
  observe(ImportProcess::Inner + 7, ImportProcess::InitialSP - 16);
  exported(ImportProcess::Outer);
  const auto Imports = Observer.takeImports();
  ASSERT_EQ(Imports.size(), 1u);
  EXPECT_EQ(Imports.front().InstructionAddress, ImportProcess::Outer);
}

TEST_F(ProcessImports, NestedInvocationsWithdrawEarlierInnerEvidence) {
  start();
  observe(ImportProcess::Inner);
  exported(ImportProcess::Inner);
  ++Process.NativeCalls;
  observe(ImportProcess::Outer);
  observe(ImportProcess::Inner, ImportProcess::InitialSP - 24);
  exported(ImportProcess::Outer);
  const auto Imports = Observer.takeImports();
  ASSERT_EQ(Imports.size(), 1u);
  EXPECT_EQ(Imports.front().InstructionAddress, ImportProcess::Outer);
}

TEST_F(ProcessImports, AnUnrelatedCandidateCannotHideAnObservedCall) {
  constexpr uint64_t Unrelated = 32;
  Process.Bytes[Unrelated] = 0xe8;
  llvm::support::endian::write32le(Process.Bytes.data() + Unrelated + 1,
                                   384 - (Unrelated + 5));
  const std::array<uint64_t, 1> Returns = {ImportProcess::Inner + 6};
  ImportObserver O{Returns, Gates};
  llvm::cantFail(O.started(Process));
  Process.stop(Unrelated, ImportProcess::InitialSP);
  llvm::cantFail(O.watched(Process, Unrelated));
  Process.stop(ImportProcess::Inner, ImportProcess::InitialSP - 8);
  llvm::cantFail(O.watched(Process, ImportProcess::Inner));
  Process.stop(ImportProcess::Gate, ImportProcess::InitialSP - 16);
  llvm::support::endian::write64le(Process.Bytes.data() +
                                       ImportProcess::InitialSP - 16,
                                   ImportProcess::Inner + 6);
  llvm::cantFail(O.watched(Process, ImportProcess::Gate));
  const auto Imports = O.takeImports();
  ASSERT_EQ(Imports.size(), 1u);
  EXPECT_EQ(Imports.front().InstructionAddress, ImportProcess::Inner);
}

TEST_F(ProcessImports, NestedStopsCannotHidePersistentEffects) {
  for (unsigned Effect = 0; Effect != 4; ++Effect) {
    SCOPED_TRACE(Effect);
    ImportProcess P;
    ImportObserver O{Continuations, Gates};
    llvm::cantFail(O.started(P));
    llvm::cantFail(O.watched(P, ImportProcess::Outer));
    P.stop(ImportProcess::Inner, ImportProcess::InitialSP - 24);
    llvm::cantFail(O.watched(P, ImportProcess::Inner));
    if (Effect == 0)
      P.Bytes[2048] = 1;
    else if (Effect == 1)
      P.Bytes[ImportProcess::InitialSP + 8] = 1;
    else if (Effect == 2)
      P.Registers[CPURegister::X64FLAGS] = {1, 0};
    else
      ++P.NativeCalls;
    P.exported(ImportProcess::Outer);
    llvm::cantFail(O.watched(P, ImportProcess::Gate));
    EXPECT_TRUE(O.takeImports().empty());
  }
}

TEST_F(ProcessImports, IncompleteAndRecursiveInvocationsWithdrawEvidence) {
  for (unsigned End = 0; End != 3; ++End) {
    SCOPED_TRACE(End);
    ImportProcess P;
    ImportObserver O{Continuations, Gates};
    llvm::cantFail(O.started(P));
    llvm::cantFail(O.watched(P, ImportProcess::Outer));
    P.exported(ImportProcess::Outer);
    llvm::cantFail(O.watched(P, ImportProcess::Gate));
    P.stop(ImportProcess::Outer, ImportProcess::InitialSP);
    llvm::cantFail(O.watched(P, ImportProcess::Outer));
    if (End == 0) {
      P.stop(ImportProcess::Outer, ImportProcess::InitialSP - 8);
      llvm::cantFail(O.watched(P, ImportProcess::Outer));
    } else if (End == 1)
      llvm::cantFail(O.invoking(P));
    EXPECT_TRUE(O.takeImports().empty());
  }
}
} // namespace
} // namespace neverd::unpack
