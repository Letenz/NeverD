//===- RAMTransactionTests.cpp - Staged RAM rollback and publication ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/CheckedAArch64Backend.h"
#include "arch/x86_64/CheckedX64Backend.h"
#include "arch/x86_64/X64Exception.h"
#include "core/ExecutionDiagnostics.h"
#include "core/RAMTransaction.h"
#include "gtest/gtest.h"

#include "llvm/Support/Endian.h"

#include <future>
#include <stdexcept>

namespace neverd::emulation {
namespace {
#define NEVERD_RAM_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_RAM_TEST_BYTES(Name, ...)                                       \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "RAMTransactionCases.def"
#undef NEVERD_RAM_TEST_BYTES
#undef NEVERD_RAM_TEST_VALUE

void nativeWrite(MemoryProjection &Memory, uint64_t Address, unsigned Size) {
  for (unsigned N = 0; N < Size; ++N) {
    const uint64_t A = Address + N;
    const auto &P = Memory.mappings().at(A & ~(memory::PageSize - 1));
    *Memory.physicalPointer(P.Physical + A % memory::PageSize) = AfterByte;
  }
}
void expectError(llvm::Expected<std::unique_ptr<RAMTransaction>> Result,
                 const char *Text) {
  ASSERT_FALSE(bool(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()), Text);
}

class RAMEffects : public testing::Test {
protected:
  std::unique_ptr<MemoryProjection> Memory;
  bool Running = false;
  void SetUp() override {
    Memory = llvm::cantFail(MemoryProjection::create(Limit));
    for (uint64_t A : {Data, Data + memory::PageSize})
      llvm::cantFail(
          Memory->map(A, memory::PageSize, Read | Write | UserAccessible));
    llvm::cantFail(Memory->addressSpace()->mapAlias(
        Alias, Data, memory::PageSize, Read | Write | UserAccessible));
    std::vector<uint8_t> Bytes(2 * memory::PageSize, BeforeByte);
    llvm::cantFail(Memory->write(Data, Bytes));
  }
  void TearDown() override {
    if (Running)
      Memory->endRun();
  }
  void start() {
    llvm::cantFail(Memory->beginRun());
    Running = true;
  }
  void expectBytes(uint64_t Address, unsigned Size, uint8_t Value) {
    std::vector<uint8_t> Bytes(Size);
    llvm::cantFail(Memory->read(Address, Bytes));
    EXPECT_EQ(Bytes, std::vector<uint8_t>(Size, Value));
  }
};

TEST_F(RAMEffects, DestructionRestoresOnlyTheDeclaredPhysicalFootprint) {
  start();
  {
    const RAMWriteRange Range{Data + memory::PageSize - WordBytes, PairBytes};
    auto T = llvm::cantFail(RAMTransaction::create(*Memory, Range, PairBytes));
    nativeWrite(*Memory, Range.Address, PairBytes);
  }
  expectBytes(Data, 2 * memory::PageSize, BeforeByte);
}

TEST_F(RAMEffects, AliasesShareOneBudgetAndStageBeforeCommitting) {
  start();
  const uint64_t Address = Data + memory::PageSize - WordBytes;
  const RAMWriteRange Ranges[] = {
      {Address, PairBytes}, {Alias + memory::PageSize - WordBytes, WordBytes}};
  auto T = llvm::cantFail(RAMTransaction::create(*Memory, Ranges, PairBytes));
  nativeWrite(*Memory, Address, PairBytes);
  llvm::cantFail(T->stage());
  expectBytes(Address, PairBytes, BeforeByte);
  expectBytes(Alias + memory::PageSize - WordBytes, WordBytes, BeforeByte);
  std::array<uint8_t, PairBytes> Staged;
  llvm::cantFail(T->read(Address, Staged));
  for (auto Byte : Staged)
    EXPECT_EQ(Byte, AfterByte);
  llvm::cantFail(T->commit());
  expectBytes(Address, PairBytes, AfterByte);
  expectBytes(Address - WordBytes, WordBytes, BeforeByte);
  expectBytes(Address + PairBytes, WordBytes, BeforeByte);
  T.reset();
  expectBytes(Address, PairBytes, AfterByte);
}

TEST_F(RAMEffects, WrongPhaseAndPartialResultReadsCannotPublish) {
  start();
  const RAMWriteRange Range{Data, WordBytes};
  auto T = llvm::cantFail(RAMTransaction::create(*Memory, Range, WordBytes));
  EXPECT_EQ(llvm::toString(T->commit()), diagnostic::RAMTransactionPhase);
  nativeWrite(*Memory, Data, WordBytes);
  llvm::cantFail(T->stage());
  EXPECT_EQ(llvm::toString(T->stage()), diagnostic::RAMTransactionPhase);
  std::array<uint8_t, PairBytes> Bytes;
  Bytes.fill(BeforeByte);
  EXPECT_EQ(llvm::toString(T->read(Data, Bytes)),
            diagnostic::RAMTransactionRange);
  for (auto Byte : Bytes)
    EXPECT_EQ(Byte, BeforeByte);
  T.reset();
  expectBytes(Data, PairBytes, BeforeByte);
}

TEST_F(RAMEffects, WriteOnlyRAMCanBeRestoredWithoutInventingReadPermission) {
  llvm::cantFail(Memory->protect(Data, memory::PageSize, Write));
  start();
  const RAMWriteRange Range{Data, WordBytes};
  auto T = llvm::cantFail(RAMTransaction::create(*Memory, Range, WordBytes));
  nativeWrite(*Memory, Data, WordBytes);
  llvm::cantFail(T->stage());
  std::array<uint8_t, WordBytes> Bytes;
  llvm::cantFail(T->read(Data, Bytes));
  for (auto Byte : Bytes)
    EXPECT_EQ(Byte, AfterByte);
  T.reset();
  Memory->endRun();
  Running = false;
  llvm::cantFail(Memory->protect(Data, memory::PageSize, Read | Write));
  expectBytes(Data, WordBytes, BeforeByte);
}

TEST_F(RAMEffects, MissingLeaseAndCrossThreadAccessAreRejected) {
  const RAMWriteRange Range{Data, WordBytes};
  expectError(RAMTransaction::create(*Memory, Range, WordBytes),
              diagnostic::RAMTransactionLease);
  start();
  auto T = llvm::cantFail(RAMTransaction::create(*Memory, Range, WordBytes));
  auto Other = std::async(std::launch::async, [&] {
    auto R = RAMTransaction::create(*Memory, Range, WordBytes);
    if (R)
      return std::string();
    return llvm::toString(R.takeError());
  });
  EXPECT_EQ(Other.get(), diagnostic::Running);
  auto Read = std::async(std::launch::async, [&] {
    std::array<uint8_t, WordBytes> B;
    return llvm::toString(Memory->addressSpace()->read(Data, B));
  });
  EXPECT_EQ(Read.get(), diagnostic::Running);
}

TEST_F(RAMEffects, InvalidRangesPermissionsDevicesAndBudgetsHaveNoEffects) {
  unsigned DeviceEffects = 0;
  GuestMMIOCallbacks IO;
  IO.Validate = [](uint64_t, uint64_t, bool) { return llvm::Error::success(); };
  IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
    ++DeviceEffects;
    return AfterWord;
  };
  IO.Write = [&](uint64_t, unsigned, uint64_t) {
    ++DeviceEffects;
    return llvm::Error::success();
  };
  llvm::cantFail(
      Memory->addressSpace()->mapMMIO(Device, memory::PageSize, std::move(IO)));
  llvm::cantFail(Memory->protect(Data, memory::PageSize, Read | Write));
  start();
  for (RAMWriteRange R : {RAMWriteRange{Data, 0},
                          {UINT64_MAX, PairBytes},
                          {Unmapped, WordBytes},
                          {Device, WordBytes}})
    expectError(RAMTransaction::create(*Memory, R, PairBytes),
                diagnostic::RAMTransactionRange);
  const RAMWriteRange Range{Data, WordBytes};
  expectError(
      RAMTransaction::create(*Memory, Range, WordBytes, Write | UserAccessible),
      diagnostic::RAMTransactionRange);
  const RAMWriteRange Large{Data, PairBytes};
  expectError(RAMTransaction::create(*Memory, Large, WordBytes),
              diagnostic::RAMTransactionRange);
  const RAMWriteRange Two[] = {{Data, WordBytes},
                               {Data + memory::PageSize, WordBytes}};
  expectError(RAMTransaction::create(*Memory, Two, WordBytes),
              diagnostic::RAMTransactionBudget);
  expectBytes(Data, 2 * memory::PageSize, BeforeByte);
  EXPECT_EQ(DeviceEffects, 0u);
}

struct InjectedEffects {
  MemoryProjection *Memory = nullptr;
  std::function<llvm::Error()> Finish;
  unsigned Size = WordBytes, Entries = 0;
  void write() {
    ++Entries;
    nativeWrite(*Memory, Data, Size);
  }
};
class WritingX64Machine final : public X64Machine {
public:
  explicit WritingX64Machine(InjectedEffects &Effects) : Effects(Effects) {}
  llvm::Error step(X64MachineState &S, uint64_t, MachineRunControl) override {
    Effects.write();
    S.reg(X64Register::AX) = EditedAccumulator;
    S.MXCSR |= FPInvalidStatus;
    S.reg(X64Register::PC) = Code + sizeof(StoreX64);
    return Effects.Finish();
  }

private:
  InjectedEffects &Effects;
};
class WritingARMMachine final : public AArch64Machine {
public:
  explicit WritingARMMachine(InjectedEffects &Effects) : Effects(Effects) {}
  llvm::Error step(AArch64MachineState &S, MachineRunControl) override {
    Effects.write();
    S.reg(AArch64Register::X1) = EditedAccumulator;
    S.reg(AArch64Register::PC) = Code + aarch64::InstructionBytes;
    return Effects.Finish();
  }

private:
  InjectedEffects &Effects;
};
class RAMMachineFailure : public testing::TestWithParam<GuestArchitecture> {
protected:
  InjectedEffects Effects;
  std::unique_ptr<ExecutionBackend> CPU;
  std::shared_ptr<AddressSpace> Space;
  CPURegister Accumulator = CPURegister::Invalid;
  void SetUp() override {
    auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
    Effects.Memory = Memory.get();
    if (GetParam() == GuestArchitecture::X64) {
      CPU = llvm::cantFail(CheckedX64Backend::create(
          std::move(Memory), std::make_unique<WritingX64Machine>(Effects)));
      llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
      llvm::cantFail(CPU->write(Code, StoreX64));
      llvm::cantFail(CPU->setReg(X64Register::CX, Data));
      Accumulator = CPURegister::X64AX;
    } else {
      CPU = llvm::cantFail(CheckedAArch64Backend::create(
          std::move(Memory), std::make_unique<WritingARMMachine>(Effects)));
      llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
      std::array<uint8_t, aarch64::InstructionBytes> Bytes;
      llvm::support::endian::write32le(Bytes.data(), PairStoreARM);
      llvm::cantFail(CPU->write(Code, Bytes));
      llvm::cantFail(CPU->setReg(AArch64Register::X0, Data));
      Effects.Size = PairBytes;
      Accumulator = CPURegister::AArch64X1;
    }
    llvm::cantFail(CPU->writeRegister(Accumulator, {InitialAccumulator, 0}));
    llvm::cantFail(CPU->map(Data, memory::PageSize, Read | Write));
    std::vector<uint8_t> Bytes(memory::PageSize, BeforeByte);
    llvm::cantFail(CPU->write(Data, Bytes));
    Space = CPU->addressSpace();
  }
  void expectUnchanged() {
    std::array<uint8_t, PairBytes> Bytes;
    llvm::cantFail(Space->read(Data, Bytes));
    for (auto Byte : Bytes)
      EXPECT_EQ(Byte, BeforeByte);
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(Accumulator))[0],
              InitialAccumulator);
    if (GetParam() == GuestArchitecture::X64)
      EXPECT_EQ(llvm::cantFail(CPU->readRegister(CPURegister::X64MXCSR))[0],
                x64::InitialMXCSR);
  }
};

TEST_P(RAMMachineFailure, UncertainTransportFailureCannotPublishRAMOrCPU) {
  Effects.Finish = [] { return diagnostic::error(diagnostic::KvmRun); };
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
  EXPECT_EQ(Effects.Entries, 1u);
  expectUnchanged();
}

TEST_P(RAMMachineFailure, CancelledCompletedEntryDiscardsRAMAndCPU) {
  Effects.Finish = [&] {
    CPU->stop();
    return llvm::Error::success();
  };
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::Stopped);
  EXPECT_TRUE(Exit.StopRequested);
  expectUnchanged();
}

TEST_P(RAMMachineFailure, ThrowingTransportDiscardsSpeculativeEffects) {
  Effects.Finish = []() -> llvm::Error {
    throw std::runtime_error(diagnostic::KvmRun);
  };
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::BackendFailure);
  expectUnchanged();
}

class FaultingX64Machine final : public X64Machine {
public:
  explicit FaultingX64Machine(InjectedEffects &Effects) : Effects(Effects) {}
  llvm::Error step(X64MachineState &S, uint64_t, MachineRunControl) override {
    Effects.write();
    S.MXCSR |= FPInvalidStatus;
    return llvm::make_error<X64ExceptionError>(X64Exception{
        unsigned(x64::ExceptionVector::SIMD), std::nullopt, std::nullopt});
  }

private:
  InjectedEffects &Effects;
};

TEST(RAMFaultDelivery, OSSeesRestoredRAMAndArchitecturalExceptionStatus) {
  InjectedEffects Effects;
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  Effects.Memory = Memory.get();
  auto CPU = llvm::cantFail(CheckedX64Backend::create(
      std::move(Memory), std::make_unique<FaultingX64Machine>(Effects)));
  llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
  llvm::cantFail(CPU->map(Data, memory::PageSize, Read | Write));
  llvm::cantFail(CPU->write(Code, StoreX64));
  llvm::cantFail(CPU->writeInteger(Data, BeforeWord, WordBytes));
  llvm::cantFail(CPU->setReg(X64Register::CX, Data));
  llvm::cantFail(CPU->setReg(X64Register::AX, InitialAccumulator));
  bool Delivered = false;
  BackendHooks Hooks;
  Hooks.RecoverableFault = [&](const BackendFault &F) {
    EXPECT_EQ(F.Interrupt, unsigned(x64::ExceptionVector::SIMD));
    EXPECT_EQ(F.PC, Code);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), BeforeWord);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), InitialAccumulator);
    EXPECT_EQ(llvm::cantFail(CPU->readRegister(CPURegister::X64MXCSR))[0],
              x64::InitialMXCSR | FPInvalidStatus);
    Delivered = true;
    return true;
  };
  llvm::cantFail(CPU->installHooks(std::move(Hooks)));
  auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
  EXPECT_TRUE(Delivered);
  EXPECT_EQ(Effects.Entries, 1u);
  EXPECT_TRUE(CPU->takeRecoverableFault());
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), BeforeWord);
}

TEST(RAMFrameEntry, TransportFailureCancellationAndExceptionsRollback) {
  for (unsigned Outcome = 0; Outcome != 3; ++Outcome) {
    InjectedEffects Effects;
    auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
    Effects.Memory = Memory.get();
    auto CPU = llvm::cantFail(CheckedX64Backend::create(
        std::move(Memory), std::make_unique<WritingX64Machine>(Effects)));
    llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
    llvm::cantFail(CPU->map(Data, memory::PageSize, Read | Write));
    llvm::cantFail(CPU->write(Code, EnterX64));
    llvm::cantFail(CPU->writeInteger(Data, BeforeWord, WordBytes));
    llvm::cantFail(CPU->setReg(X64Register::AX, InitialAccumulator));
    llvm::cantFail(CPU->setReg(X64Register::SP, Data + WordBytes));
    llvm::cantFail(CPU->setReg(X64Register::BP, Data + PairBytes));
    Effects.Finish = [&]() -> llvm::Error {
      if (!Outcome)
        return diagnostic::error(diagnostic::KvmRun);
      if (Outcome == 1) {
        CPU->stop();
        return llvm::Error::success();
      }
      throw std::runtime_error(diagnostic::KvmRun);
    };
    const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
    EXPECT_EQ(Exit.Kind, Outcome == 1 ? ExecutionExitKind::Stopped
                                      : ExecutionExitKind::BackendFailure);
    EXPECT_EQ(Effects.Entries, 1u);
    std::array<uint8_t, WordBytes> Bytes{};
    llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
    for (auto Byte : Bytes)
      EXPECT_EQ(Byte, BeforeByte);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), InitialAccumulator);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::BP)), Data + PairBytes);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SP)), Data + WordBytes);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  }
}

class FaultingEnterMachine final : public X64Machine {
public:
  explicit FaultingEnterMachine(InjectedEffects &Effects) : Effects(Effects) {}
  llvm::Error step(X64MachineState &, uint64_t, MachineRunControl) override {
    Effects.write();
    return llvm::make_error<X64ExceptionError>(X64Exception{
        unsigned(x64::ExceptionVector::PageFault), std::nullopt, Data});
  }

private:
  InjectedEffects &Effects;
};
TEST(RAMFrameEntry, ProcessorFaultPreservesItsCompletedRAMBeforeOSDelivery) {
  InjectedEffects Effects;
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  Effects.Memory = Memory.get();
  auto CPU = llvm::cantFail(CheckedX64Backend::create(
      std::move(Memory), std::make_unique<FaultingEnterMachine>(Effects)));
  llvm::cantFail(CPU->map(Code, memory::PageSize, Read | Write | Execute));
  llvm::cantFail(CPU->map(Data, memory::PageSize, Read | Write));
  llvm::cantFail(CPU->write(Code, EnterX64));
  llvm::cantFail(CPU->writeInteger(Data, BeforeWord, WordBytes));
  llvm::cantFail(CPU->setReg(X64Register::SP, Data + WordBytes));
  llvm::cantFail(CPU->setReg(X64Register::BP, Data + PairBytes));
  unsigned Delivered = 0;
  BackendHooks H;
  H.RecoverableFault = [&](const BackendFault &F) {
    ++Delivered;
    EXPECT_EQ(F.Interrupt, unsigned(x64::ExceptionVector::PageFault));
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), AfterWord);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::BP)), Data + PairBytes);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SP)), Data + WordBytes);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
    return true;
  };
  llvm::cantFail(CPU->installHooks(std::move(H)));
  const auto Exit = llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault);
  EXPECT_EQ(Effects.Entries, 1u);
  EXPECT_EQ(Delivered, 1u);
  ASSERT_TRUE(CPU->takeRecoverableFault());
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), AfterWord);
}

INSTANTIATE_TEST_SUITE_P(Architectures, RAMMachineFailure,
                         testing::Values(GuestArchitecture::X64,
                                         GuestArchitecture::AArch64));
} // namespace
} // namespace neverd::emulation
