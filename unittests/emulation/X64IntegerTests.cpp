//===- X64IntegerTests.cpp - Scalar integer widths and implicit outputs -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

namespace neverd::emulation {
namespace {
#define NEVERD_INTEGER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_INTEGER_MULTIPLY(Name, Width, Memory, ...)                      \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_INTEGER_EXTEND(Name, Input, Output, High, ...)                  \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_EXTEND
#undef NEVERD_INTEGER_MULTIPLY
#undef NEVERD_INTEGER_VALUE
struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  bool User;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
const Parameter Parameters[] = {
#define NEVERD_INTEGER_BACKEND(Name, Backend, User)                            \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_BACKEND
};
struct Multiply {
  const char *Name;
  unsigned Size;
  bool Memory;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Multiply Multiplies[] = {
#define NEVERD_INTEGER_MULTIPLY(Name, Width, Memory, ...)                      \
  {#Name, Width, Memory, Name},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_MULTIPLY
};
struct Product {
  unsigned Size;
  bool Overflow;
  uint64_t Input, Low, High;
};
const Product Products[] = {
#define NEVERD_INTEGER_PRODUCT(Width, Overflow, Input, Low, High)              \
  {Width, Overflow, Input, Low, High},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_PRODUCT
};
struct Extend {
  const char *Name;
  uint64_t Input, Low, High;
  llvm::ArrayRef<uint8_t> Bytes;
};
const Extend Extensions[] = {
#define NEVERD_INTEGER_EXTEND(Name, Input, Low, High, ...)                     \
  {#Name, Input, Low, High, Name},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_EXTEND
};
class X64Integer : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
  }
  void seed(uint64_t Accumulator) {
    llvm::cantFail(CPU->setReg(X64Register::AX, Accumulator));
    llvm::cantFail(CPU->setReg(X64Register::DX, HighSeed));
    llvm::cantFail(CPU->setReg(X64Register::BX, Multiplier));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
  }
  uint64_t reg(X64Register R) { return llvm::cantFail(CPU->reg(R)); }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Bytes, BackendHooks Hooks = {}) {
    llvm::cantFail(CPU->write(Code, Bytes));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
};
TEST_P(X64Integer, MultiplyPreservesWidthsAndDefinedFlags) {
  for (const auto &Instruction : Multiplies)
    for (const auto &Expected : Products) {
      if (Expected.Size != Instruction.Size)
        continue;
      SCOPED_TRACE(Instruction.Name);
      SCOPED_TRACE(Expected.Overflow);
      seed(Expected.Input);
      llvm::cantFail(CPU->writeInteger(Data, Multiplier, Instruction.Size));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t Address, uint32_t Size) {
        EXPECT_EQ(Address, Data);
        EXPECT_EQ(Size, Instruction.Size);
        ++Reads;
      };
      H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
      const auto Exit = run(Instruction.Bytes, std::move(H));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
      EXPECT_EQ(reg(X64Register::AX), Expected.Low);
      EXPECT_EQ(reg(X64Register::DX), Expected.High);
      EXPECT_EQ(reg(X64Register::BX), Multiplier);
      EXPECT_EQ(reg(X64Register::CX), Data);
      // Only CF and OF are architecturally defined for MUL.
      EXPECT_EQ(reg(X64Register::FLAGS) & CarryOverflow,
                Expected.Overflow ? CarryOverflow : 0);
      EXPECT_EQ(reg(X64Register::PC), Code + Instruction.Bytes.size());
      EXPECT_EQ(Reads, unsigned(Instruction.Memory));
      EXPECT_EQ(Writes, 0u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Instruction.Size)),
                Multiplier);
    }
}
TEST_P(X64Integer, SignExtensionKeepsFlagsAndUntouchedRegisterBits) {
  for (const auto &Instruction : Extensions) {
    SCOPED_TRACE(Instruction.Name);
    seed(Instruction.Input);
    const auto Exit = run(Instruction.Bytes);
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(reg(X64Register::AX), Instruction.Low);
    EXPECT_EQ(reg(X64Register::DX), Instruction.High);
    EXPECT_EQ(reg(X64Register::BX), Multiplier);
    EXPECT_EQ(reg(X64Register::CX), Data);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code + Instruction.Bytes.size());
  }
}
TEST_P(X64Integer, ReadObserverStopsBeforeImplicitRegisterEffects) {
  for (const auto &Instruction : Multiplies) {
    if (!Instruction.Memory)
      continue;
    SCOPED_TRACE(Instruction.Name);
    seed(Multiplier);
    llvm::cantFail(CPU->writeInteger(Data, Multiplier, Instruction.Size));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, Instruction.Size);
      ++Reads;
      CPU->stop();
    };
    const auto Exit = run(Instruction.Bytes, std::move(H));
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(reg(X64Register::AX), Multiplier);
    EXPECT_EQ(reg(X64Register::DX), HighSeed);
    EXPECT_EQ(reg(X64Register::FLAGS), Flags);
    EXPECT_EQ(reg(X64Register::PC), Code);
  }
}
TEST_P(X64Integer, MissingSourcePagePreservesBothProductHalves) {
  seed(Multiplier);
  const auto Address = Data + PageSize - sizeof(uint64_t) + 1;
  llvm::cantFail(CPU->setReg(X64Register::CX, Address));
  const auto Exit = run(MulMemory64);
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(reg(X64Register::AX), Multiplier);
  EXPECT_EQ(reg(X64Register::DX), HighSeed);
  EXPECT_EQ(reg(X64Register::FLAGS), Flags);
  EXPECT_EQ(reg(X64Register::PC), Code);
}
INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64Integer,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
