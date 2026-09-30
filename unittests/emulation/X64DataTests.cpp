//===- X64DataTests.cpp - Checked native scalar and SSE2 state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/x86_64/X64Machine.h"
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Memory.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_DATA_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DATA_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#define NEVERD_DATA_TEXT(Name, Text) constexpr char Name[] = Text;
#include "X64DataCases.def"
#undef NEVERD_DATA_BYTES
#undef NEVERD_DATA_TEXT
#undef NEVERD_DATA_VALUE

using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
class X64Data : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override {
    auto B =
        createExecutionBackend(std::get<0>(GetParam()), std::get<1>(GetParam()),
                               Limit, GuestArchitecture::X64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(CPU->setReg(X64Register::CX, Data));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Bytes, BackendHooks Hooks = {}) {
    std::vector<uint8_t> CodeBytes(Bytes.begin(), Bytes.end());
    CodeBytes.push_back(NopOpcode);
    llvm::cantFail(CPU->write(Code, CodeBytes));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  void expectStopped(const ExecutionExit &Exit) {
    ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
  }
  void seedVector() {
    llvm::cantFail(CPU->setXmm(0, {Low, High}));
    llvm::cantFail(CPU->writeInteger(Data, Low, WordBytes));
    llvm::cantFail(CPU->writeInteger(Data + WordBytes, High, WordBytes));
  }
};

TEST_P(X64Data, MemoryArithmeticPreviewMatchesNativeBytesAndDefinedFlags) {
  auto Check = [&](const char *Name, unsigned Size, uint64_t Before,
                   uint64_t Operand, uint64_t After, uint64_t Flags,
                   llvm::ArrayRef<uint8_t> Bytes) {
    SCOPED_TRACE(Name);
    for (bool Stop : {true, false}) {
      llvm::cantFail(CPU->writeInteger(Data, Before, Size));
      llvm::cantFail(CPU->writeInteger(Data + Size, High, WordBytes));
      llvm::cantFail(CPU->setReg(X64Register::DX, Operand));
      llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t A, uint32_t N) {
        EXPECT_EQ(A, Data);
        EXPECT_EQ(N, Size);
        ++Reads;
      };
      H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(A, Data);
        EXPECT_EQ(N, Size);
        EXPECT_EQ(V, After);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Size)), Before);
        ++Writes;
        if (Stop)
          CPU->stop();
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(Writes, 1u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, Size)),
                Stop ? Before : After);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + Size, WordBytes)), High);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & ArithmeticFlags,
                Stop ? InitialFlags & ArithmeticFlags : Flags);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)),
                Stop ? Code : Code + Bytes.size());
    }
  };
#define NEVERD_DATA_UPDATE(Name, Size, Before, Operand, After, Flags, ...)     \
  Check(#Name, Size, Before, Operand, After, Flags, {__VA_ARGS__});
#include "X64DataCases.def"
#undef NEVERD_DATA_UPDATE
}

TEST_P(X64Data, MemorySetConditionCoversEveryFlagCombinationWithoutReading) {
  auto Check = [&](uint32_t Truth, llvm::ArrayRef<uint8_t> Bytes) {
    for (unsigned N = 0; N < FlagCombinations; ++N) {
      const uint64_t FlagBits[] = {Carry, Parity, Zero, Sign, Overflow};
      uint64_t Flags = InitialFlags & ~ArithmeticFlags;
      for (unsigned I = 0; I < std::size(FlagBits); ++I)
        if (N & (1u << I))
          Flags |= FlagBits[I];
      llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
      llvm::cantFail(CPU->writeInteger(Data, High, WordBytes));
      unsigned Reads = 0, Writes = 0;
      BackendHooks H;
      H.Read = [&](uint64_t, uint32_t) { ++Reads; };
      H.Write = [&](uint64_t A, uint32_t Size, uint64_t V) {
        EXPECT_EQ(A, Data);
        EXPECT_EQ(Size, 1u);
        EXPECT_EQ(V, (Truth >> N) & 1);
        ++Writes;
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 1u);
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 1)), (Truth >> N) & 1);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), Flags);
    }
  };
#define NEVERD_DATA_CONDITION(Name, Truth, ...)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Truth, {__VA_ARGS__});                                               \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_CONDITION
}

TEST_P(X64Data, RegisterBitTestMasksItsIndexAndDoesNotChangeOperands) {
  for (auto Bytes : {llvm::ArrayRef(Bit32), llvm::ArrayRef(Bit64)}) {
    const unsigned Bits = Bytes.size() == sizeof(Bit32)
                              ? sizeof(uint32_t) * CHAR_BIT
                              : sizeof(uint64_t) * CHAR_BIT;
    for (unsigned Index = 0; Index < Bits * 2; ++Index) {
      llvm::cantFail(CPU->setReg(X64Register::DX, High));
      llvm::cantFail(CPU->setReg(X64Register::CX, Index));
      expectStopped(run(Bytes));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)) & Carry,
                (High >> (Index % Bits)) & Carry);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), High);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), Index);
    }
  }
}

TEST_P(X64Data, EveryXmmRegisterSurvivesNativeExecutionAndContextRestore) {
  for (unsigned N = 0; N < XmmCount; ++N)
    llvm::cantFail(CPU->setXmm(N, {Low + N, High + N}));
  auto Saved = llvm::cantFail(CPU->saveContext());
  expectStopped(run(XorSelf));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{0, 0}));
  for (unsigned N = 0; N < XmmCount; ++N)
    llvm::cantFail(CPU->setXmm(N, {0, 0}));
  llvm::cantFail(CPU->restoreContext(*Saved));
  for (unsigned N = 0; N < XmmCount; ++N) {
    std::vector<uint8_t> Bytes(std::begin(StoreXmm), std::end(StoreXmm));
    Bytes[0] = RexBase | (N > ModRMRegisterMask ? RexHighRegister : 0);
    Bytes[ModRMOffset] =
        ModRMCX | ((N & ModRMRegisterMask) << ModRMRegisterShift);
    expectStopped(run(Bytes));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, WordBytes)), Low + N);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + WordBytes, WordBytes)),
              High + N);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(N)), (RegisterValue{Low + N, High + N}));
  }
}

TEST_P(X64Data, VectorStoresObserveEveryByteBeforeAnyEffect) {
  auto Check = [&](unsigned Size, unsigned Alignment,
                   llvm::ArrayRef<uint8_t> Bytes) {
    const uint64_t Address = Data + Alignment;
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    for (unsigned StopAt = 0; StopAt <= (Size + WordBytes - 1) / WordBytes;
         ++StopAt) {
      seedVector();
      llvm::cantFail(CPU->writeInteger(Address, 0, WordBytes));
      llvm::cantFail(CPU->writeInteger(Address + WordBytes, 0, WordBytes));
      unsigned Writes = 0;
      BackendHooks H;
      H.Write = [&](uint64_t A, uint32_t N, uint64_t V) {
        EXPECT_EQ(A, Address + Writes * WordBytes);
        EXPECT_EQ(N, std::min<uint64_t>(Size, WordBytes));
        EXPECT_EQ(V, Writes
                         ? High
                         : Low & (UINT64_MAX >> ((WordBytes - N) * CHAR_BIT)));
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)), 0u);
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            0u);
        if (++Writes == StopAt)
          CPU->stop();
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      if (StopAt) {
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(Address, WordBytes)), 0u);
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            0u);
        EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
      } else {
        EXPECT_EQ(Writes, (Size + WordBytes - 1) / WordBytes);
        EXPECT_EQ(llvm::cantFail(CPU->readInteger(
                      Address, std::min<uint64_t>(Size, WordBytes))),
                  Low & (UINT64_MAX >>
                         ((WordBytes - std::min<uint64_t>(Size, WordBytes)) *
                          CHAR_BIT)));
        EXPECT_EQ(
            llvm::cantFail(CPU->readInteger(Address + WordBytes, WordBytes)),
            Size > WordBytes ? High : 0);
      }
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
    }
  };
#define NEVERD_DATA_STORE(Name, Size, Alignment, ...)                          \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Size, Alignment, {__VA_ARGS__});                                     \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_STORE
}

TEST_P(X64Data, VectorLoadsAndRegisterMovesPreserveTheArchitecturalUpperBits) {
  auto Check = [&](unsigned Size, RegisterValue Expected,
                   llvm::ArrayRef<uint8_t> Bytes) {
    seedVector();
    llvm::cantFail(CPU->setXmm(1, {High, Low}));
    llvm::cantFail(CPU->setReg(X64Register::DX, High));
    unsigned Reads = 0;
    BackendHooks H;
    H.Read = [&](uint64_t A, uint32_t N) {
      EXPECT_EQ(A, Data);
      EXPECT_EQ(N, Size);
      ++Reads;
    };
    expectStopped(run(Bytes, std::move(H)));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(Reads, Size ? 1u : 0u);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), Expected);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(1)), (RegisterValue{High, Low}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
  };
#define NEVERD_DATA_LOAD(Name, Size, L, H, ...)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Size, {L, H}, {__VA_ARGS__});                                        \
  }
#define NEVERD_DATA_REGISTER(Name, L, H, ...)                                  \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(0, {L, H}, {__VA_ARGS__});                                           \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_REGISTER
#undef NEVERD_DATA_LOAD
}

TEST_P(X64Data, VectorStorePermissionFaultPreservesBothWords) {
  seedVector();
  llvm::cantFail(CPU->protect(Data, PageSize, Read | UserAccessible));
  auto Exit = run(StoreXmm);
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Write);
  std::array<uint8_t, VectorBytes> Bytes{};
  llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
  for (unsigned N = 0; N < WordBytes; ++N) {
    EXPECT_EQ(Bytes[N], uint8_t(Low >> (N * CHAR_BIT)));
    EXPECT_EQ(Bytes[N + WordBytes], uint8_t(High >> (N * CHAR_BIT)));
  }
  EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{Low, High}));
}

TEST_P(X64Data, MaskedSSEArithmeticMatchesIndependentHostExecution) {
#if defined(__x86_64__) || defined(_M_X64)
#define NEVERD_FP_BYTES(Name, ...) constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64FPCases.def"
#undef NEVERD_FP_BYTES
  struct Input {
    uint64_t DoubleA, DoubleB;
    uint32_t FloatA, FloatB;
  };
  const Input Inputs[] = {
#define NEVERD_SSE_INPUT(Name, DA, DB, FA, FB) {DA, DB, FA, FB},
#include "X64DataCases.def"
#undef NEVERD_SSE_INPUT
  };
  auto Check = [&](unsigned Width, bool Double,
                   llvm::ArrayRef<uint8_t> Instruction) {
#ifdef _WIN32
    std::vector<uint8_t> Oracle(std::begin(Win64Before), std::end(Win64Before));
    const auto After = llvm::ArrayRef<uint8_t>(Win64After);
#else
    std::vector<uint8_t> Oracle(std::begin(SysVBefore), std::end(SysVBefore));
    const auto After = llvm::ArrayRef<uint8_t>(SysVAfter);
#endif
    Oracle.insert(Oracle.end(), Instruction.begin(), Instruction.end());
    Oracle.insert(Oracle.end(), After.begin(), After.end());
    std::error_code EC;
    auto Block = llvm::sys::Memory::allocateMappedMemory(
        PageSize, nullptr,
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    auto Release = llvm::scope_exit(
        [&] { (void)llvm::sys::Memory::releaseMappedMemory(Block); });
    std::memcpy(Block.base(), Oracle.data(), Oracle.size());
    EC = llvm::sys::Memory::protectMappedMemory(
        Block, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_EXEC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    llvm::sys::Memory::InvalidateInstructionCache(Block.base(), Oracle.size());
    auto Execute = reinterpret_cast<void (*)(void *, void *, void *, void *)>(
        Block.base());
    for (const auto &I : Inputs) {
      RegisterValue A, B;
      if (Double) {
        A = {I.DoubleA, I.DoubleA};
        B = {I.DoubleB, I.DoubleB};
      } else {
        const uint64_t FA = I.FloatA | (uint64_t(I.FloatA) << FloatLaneShift);
        const uint64_t FB = I.FloatB | (uint64_t(I.FloatB) << FloatLaneShift);
        A = {FA, FA};
        B = {FB, FB};
      }
      for (uint64_t Rounding :
           {uint64_t(0), RoundingDown, RoundingUp, RoundingTruncate}) {
        for (uint64_t Flush : {uint64_t(0), FlushToZero}) {
          X64MachineState Seed;
          Seed.Xmm[0] = A;
          Seed.Xmm[1] = B;
          Seed.MXCSR = InitialMXCSR | Rounding | Flush;
          alignas(x64::fp::RegisterSlotBytes)
              std::array<uint8_t, x64::fp::LegacyBytes>
                  Input{}, Output{}, Host{};
          llvm::cantFail(encodeX64FXState(Seed, Input));
          Execute(Input.data(), Output.data(), Host.data(), nullptr);
          X64MachineState Expected;
          llvm::cantFail(decodeX64FXState(Expected, Output));
          for (bool Memory : {false, true}) {
            std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
            if (Memory)
              Bytes.back() = MemoryModRM;
            llvm::cantFail(CPU->setXmm(0, A));
            llvm::cantFail(CPU->setXmm(1, B));
            llvm::cantFail(CPU->setReg(X64Register::MXCSR, Seed.MXCSR));
            llvm::cantFail(CPU->setReg(X64Register::FLAGS, InitialFlags));
            llvm::cantFail(CPU->writeInteger(Data, B[0], WordBytes));
            llvm::cantFail(
                CPU->writeInteger(Data + WordBytes, B[1], WordBytes));
            unsigned Reads = 0;
            BackendHooks Hooks;
            Hooks.Read = [&](uint64_t Address, uint32_t Size) {
              EXPECT_EQ(Address, Data);
              EXPECT_EQ(Size, Width);
              ++Reads;
            };
            expectStopped(run(Bytes, std::move(Hooks)));
            ASSERT_FALSE(HasFatalFailure());
            EXPECT_EQ(Reads, unsigned(Memory));
            EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), Expected.Xmm[0]);
            EXPECT_EQ(llvm::cantFail(CPU->xmm(1)), B);
            EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
                      Expected.MXCSR);
            EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)),
                      InitialFlags);
          }
        }
      }
    }
  };
#define NEVERD_X64_SSE_INSTRUCTION(Name, Width, Alignment, Double, ...)        \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    Check(Width, Double, Bytes);                                               \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "arch/x86_64/X64SSEInstructions.def"
#undef NEVERD_X64_SSE_INSTRUCTION
#else
  GTEST_SKIP();
#endif
}

TEST_P(X64Data, SSEMemoryObserverStopsBeforeResultAndStatusChanges) {
  auto Check = [&](unsigned Width, llvm::ArrayRef<uint8_t> Instruction) {
    std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
    Bytes.back() = MemoryModRM;
    seedVector();
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, InitialMXCSR));
    unsigned Reads = 0;
    BackendHooks Hooks;
    Hooks.Read = [&](uint64_t Address, uint32_t Size) {
      EXPECT_EQ(Address, Data);
      EXPECT_EQ(Size, Width);
      ++Reads;
      CPU->stop();
    };
    expectStopped(run(Bytes, std::move(Hooks)));
    EXPECT_EQ(Reads, 1u);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{Low, High}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)), InitialMXCSR);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  };
#define NEVERD_X64_SSE_INSTRUCTION(Name, Width, Alignment, Double, ...)        \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    constexpr uint8_t Bytes[] = {__VA_ARGS__};                                 \
    Check(Width, Bytes);                                                       \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "arch/x86_64/X64SSEInstructions.def"
#undef NEVERD_X64_SSE_INSTRUCTION
}

TEST_P(X64Data, ScalarConversionPreservesStickyStatusAndIgnoresRoundingMode) {
  auto Check = [&](uint64_t Bits, uint64_t Integer, uint64_t Status) {
    for (uint64_t Rounding :
         {uint64_t(0), RoundingDown, RoundingUp, RoundingTruncate}) {
      llvm::cantFail(CPU->setXmm(0, {Bits, High}));
      llvm::cantFail(CPU->setReg(X64Register::MXCSR, InitialMXCSR | Rounding));
      expectStopped(run(ConvertSD));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Integer);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
                InitialMXCSR | Rounding | Status);
      EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{Bits, High}));
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), InitialFlags);
    }
  };
#define NEVERD_DATA_CONVERSION(Name, Bits, Integer, Status)                    \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Bits, Integer, Status);                                              \
    ASSERT_FALSE(HasFatalFailure());                                           \
  }
#include "X64DataCases.def"
#undef NEVERD_DATA_CONVERSION
}

TEST_P(X64Data,
       ScalarSubtractionUsesRestoredMXCSRRoundingAndPreservesUpperLane) {
  for (uint64_t Rounding :
       {uint64_t(0), RoundingDown, RoundingUp, RoundingTruncate}) {
    llvm::cantFail(CPU->setXmm(0, {DoubleOne, High}));
    llvm::cantFail(CPU->setXmm(1, {NegativeHalfULP, Low}));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR,
                               InitialMXCSR | Rounding | InvalidStatus));
    auto Saved = llvm::cantFail(CPU->saveContext());
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, InitialMXCSR));
    llvm::cantFail(CPU->restoreContext(*Saved));
    expectStopped(run(SubSD));
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)),
              (RegisterValue{DoubleOne + (Rounding == RoundingUp), High}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)),
              InitialMXCSR | Rounding | InvalidStatus | PrecisionStatus);
  }
  EXPECT_NE(llvm::toString(CPU->setReg(X64Register::MXCSR, 0)), "");
}

TEST_P(X64Data, VectorLoadReadObserverStopsBeforeRegisterChanges) {
  seedVector();
  llvm::cantFail(CPU->setXmm(0, {High, Low}));
  BackendHooks H;
  H.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  expectStopped(run(LoadXmm, std::move(H)));
  EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), (RegisterValue{High, Low}));
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
}

TEST_P(X64Data, CrossPageVectorStoreReportsTheMissingSecondPage) {
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + PageSize - WordBytes));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  auto Exit = run(StoreXmm, std::move(H));
  ASSERT_EQ(Exit.Kind, ExecutionExitKind::GuestFault) << Exit.Diagnostic;
  ASSERT_TRUE(Exit.Fault);
  EXPECT_EQ(Exit.Fault->Kind, BackendFaultKind::UnmappedMemory);
  EXPECT_EQ(Exit.Fault->Address, Data + PageSize);
  EXPECT_EQ(Exit.Fault->Size, WordBytes);
  EXPECT_EQ(Writes, 2u);
  std::array<uint8_t, WordBytes> Prefix{};
  llvm::cantFail(CPU->snapshotBacking(Data + PageSize - WordBytes, Prefix));
  EXPECT_EQ(Prefix, (std::array<uint8_t, WordBytes>{}));
}

TEST_P(X64Data, MisalignedAlignedVectorStoreRejectsBeforeObservations) {
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  EXPECT_EQ(run(AlignedVectorStore, std::move(H)).Kind,
            ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Writes, 0u);
}

TEST_P(X64Data, MisalignedLockedUpdateRejectsBeforeObservations) {
  llvm::cantFail(CPU->setReg(X64Register::CX, Data + 1));
  unsigned Writes = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Writes; };
  EXPECT_EQ(run(LockedUpdate, std::move(H)).Kind,
            ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Writes, 0u);
}

TEST_P(X64Data, RepeatCopyStopsAtRestartBoundaryAndHandlesZeroCount) {
  for (bool Backward : {false, true}) {
    for (auto Bytes :
         {llvm::ArrayRef(RepeatMove), llvm::ArrayRef(RepeatMove32)}) {
      llvm::cantFail(CPU->writeInteger(Data, Low, WordBytes));
      llvm::cantFail(CPU->writeInteger(Data + VectorBytes, 0, WordBytes));
      const uint64_t Offset = Backward ? DeviceWidth : 0;
      llvm::cantFail(CPU->setReg(X64Register::SI, Data + Offset));
      llvm::cantFail(CPU->setReg(X64Register::DI, Data + VectorBytes + Offset));
      llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
      llvm::cantFail(CPU->setReg(
          X64Register::FLAGS, InitialFlags | (Backward ? DirectionFlag : 0)));
      unsigned Writes = 0;
      BackendHooks H;
      H.Write = [&](uint64_t, uint32_t, uint64_t) {
        if (++Writes == CopyCount)
          CPU->stop();
      };
      expectStopped(run(Bytes, std::move(H)));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount - 1);
      expectStopped(run(Bytes));
      ASSERT_FALSE(HasFatalFailure());
      EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + VectorBytes, WordBytes)),
                Low);
      EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 0u);
      // No memory access occurs for a zero count, even with invalid pointers.
      llvm::cantFail(CPU->setReg(X64Register::SI, 0));
      llvm::cantFail(CPU->setReg(X64Register::DI, 0));
      expectStopped(run(Bytes));
      ASSERT_FALSE(HasFatalFailure());
    }
  }
}

class X64Device : public X64Data {
protected:
  unsigned Reads = 0, Writes = 0, Prepares = 0, Validations = 0;
  uint64_t DeviceValue = Value;
  bool FailCommit = false, ThrowRead = false;
  bool StopValidation = false, StopPreparation = false;
  GuestMMIOCallbacks callbacks() {
    GuestMMIOCallbacks IO;
    IO.Validate = [&](uint64_t, uint64_t, bool) {
      ++Validations;
      if (StopValidation)
        CPU->stop();
      return llvm::Error::success();
    };
    IO.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      if (ThrowRead)
        throw std::runtime_error(DeviceFailure);
      ++Reads;
      return DeviceValue++;
    };
    IO.Write = [&](uint64_t, unsigned, uint64_t V) {
      ++Writes;
      DeviceValue = V;
      return llvm::Error::success();
    };
    IO.PrepareRead = [&](uint64_t,
                         unsigned) -> llvm::Expected<GuestMMIOPreparedRead> {
      ++Prepares;
      if (StopPreparation)
        CPU->stop();
      return GuestMMIOPreparedRead{DeviceValue, [&]() -> llvm::Error {
                                     if (FailCommit)
                                       return llvm::createStringError(
                                           llvm::inconvertibleErrorCode(),
                                           DeviceFailure);
                                     ++Reads;
                                     ++DeviceValue;
                                     return llvm::Error::success();
                                   }};
    };
    return IO;
  }
  void mapDevice() {
    llvm::cantFail(CPU->mapMMIO(Alias, PageSize, callbacks()));
    llvm::cantFail(CPU->setReg(X64Register::CX, Alias));
  }
  void prepareCopy() {
    mapDevice();
    llvm::cantFail(CPU->setReg(X64Register::SI, Alias));
    llvm::cantFail(CPU->setReg(X64Register::DI, Data));
    llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
  }
};

TEST_P(X64Device, ScalarDeviceAccessIsOneTransactionAndNeverRAM) {
  mapDevice();
  EXPECT_NE(llvm::toString(CPU->validateBacking(Alias, DeviceWidth)), "");
  llvm::cantFail(CPU->setReg(X64Register::AX, UINT64_MAX));
  expectStopped(run(LoadDevice));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Value);
  EXPECT_EQ(Reads, 1u);
  EXPECT_EQ(Prepares, 0u);
  expectStopped(run(StoreDevice));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Writes, 1u);
  EXPECT_EQ(DeviceValue, Value);
  llvm::cantFail(CPU->unmapMMIO(Alias, PageSize));
  llvm::cantFail(CPU->map(Alias, PageSize, Read | Write));
  llvm::cantFail(CPU->writeInteger(Alias, Updated, DeviceWidth));
  expectStopped(run(LoadDevice));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), Updated);
  EXPECT_EQ(Reads, 1u);
}

TEST_P(X64Device, ReadObserverStopDoesNotConsumeDeviceRead) {
  mapDevice();
  BackendHooks H;
  H.Read = [&](uint64_t, uint32_t) { CPU->stop(); };
  expectStopped(run(LoadDevice, std::move(H)));
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(DeviceValue, Value);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
}

TEST_P(X64Device, PreparedStringReadCommitsOnlyAfterWriteObserverAdmitsIt) {
  prepareCopy();
  unsigned Observations = 0;
  BackendHooks H;
  H.Write = [&](uint64_t A, uint32_t Size, uint64_t V) {
    EXPECT_EQ(A, Data + Observations * DeviceWidth);
    EXPECT_EQ(Size, DeviceWidth);
    EXPECT_EQ(V, Value + Observations);
    if (++Observations == CopyCount)
      CPU->stop();
  };
  expectStopped(run(RepeatMove, std::move(H)));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Prepares, CopyCount);
  EXPECT_EQ(Reads, CopyCount - 1);
  EXPECT_EQ(DeviceValue, Value + 1);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, DeviceWidth)), Value);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + DeviceWidth, DeviceWidth)),
            0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), 1u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Alias + DeviceWidth);
  expectStopped(run(RepeatMove));
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(Reads, CopyCount);
  EXPECT_EQ(Prepares, CopyCount + 1);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data + DeviceWidth, DeviceWidth)),
            Value + 1);
}

TEST_P(X64Device, InvalidDestinationPreventsDevicePreparationAndEffects) {
  prepareCopy();
  llvm::cantFail(CPU->protect(Data, PageSize, Read));
  auto Exit = run(RepeatMove);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::GuestFault);
  EXPECT_EQ(Prepares, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount);
}

TEST_P(X64Device, ValidationStopPreventsPreparationAndWriteObservation) {
  prepareCopy();
  StopValidation = true;
  unsigned Observations = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observations; };
  expectStopped(run(RepeatMove, std::move(H)));
  EXPECT_EQ(Prepares, 0u);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, DeviceWidth)), 0u);
}

TEST_P(X64Device, PreparationStopPreventsWriteObservationAndReadCommit) {
  prepareCopy();
  StopPreparation = true;
  unsigned Observations = 0;
  BackendHooks H;
  H.Write = [&](uint64_t, uint32_t, uint64_t) { ++Observations; };
  expectStopped(run(RepeatMove, std::move(H)));
  EXPECT_EQ(Prepares, 1u);
  EXPECT_EQ(Observations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, DeviceWidth)), 0u);
}

TEST_P(X64Device, DeviceCommitFailureIsTerminalWithoutAdvancingOrWritingRAM) {
  prepareCopy();
  FailCommit = true;
  auto Saved = llvm::cantFail(CPU->saveContext());
  auto Exit = run(RepeatMove);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::DeviceFailure) << Exit.Diagnostic;
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), CopyCount);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::SI)), Alias);
  EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), Code);
  EXPECT_NE(llvm::toString(CPU->restoreContext(*Saved)), "");
  std::array<uint8_t, DeviceWidth> Bytes{};
  llvm::cantFail(CPU->snapshotBacking(Data, Bytes));
  EXPECT_EQ(Bytes, (std::array<uint8_t, DeviceWidth>{}));
}

TEST_P(X64Device, CallbackExceptionIsADeviceFailure) {
  mapDevice();
  ThrowRead = true;
  auto Exit = run(LoadDevice);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::DeviceFailure);
  EXPECT_EQ(Reads, 0u);
}

TEST_P(X64Device, ReadModifyWriteDeviceAccessFailsBeforeEveryDeviceCallback) {
  mapDevice();
  auto Exit = run(DeviceUpdate);
  EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Validations, 0u);
  EXPECT_EQ(Reads, 0u);
  EXPECT_EQ(Writes, 0u);
}

TEST_P(X64Device, LegacyReadCallbackCannotBeSpeculatedForStringWritePreview) {
  auto IO = callbacks();
  IO.PrepareRead = {};
  llvm::cantFail(CPU->mapMMIO(Alias, PageSize, std::move(IO)));
  llvm::cantFail(CPU->setReg(X64Register::SI, Alias));
  llvm::cantFail(CPU->setReg(X64Register::DI, Data));
  llvm::cantFail(CPU->setReg(X64Register::CX, CopyCount));
  EXPECT_EQ(run(RepeatMove).Kind, ExecutionExitKind::UnsupportedOperation);
  EXPECT_EQ(Validations, 0u);
  EXPECT_EQ(Reads, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Backends, X64Device,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::CheckedX64)));

TEST_P(X64Data, UnmodeledFormsRemainExplicitlyUnsupported) {
  // An unsupported exit is terminal, so each independent form uses a fresh CPU.
  for (auto Bytes : {llvm::ArrayRef(X87LoadZero), llvm::ArrayRef(StringMove),
                     llvm::ArrayRef(AVXMove), llvm::ArrayRef(MMXMove),
                     llvm::ArrayRef(BitMemory)}) {
    SetUp();
    ASSERT_TRUE(CPU);
    auto Exit = run(Bytes);
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
  }
}

INSTANTIATE_TEST_SUITE_P(
    Backends, X64Data,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::CheckedX64,
                                     ExecutionContract::CheckedUserX64)));
} // namespace
} // namespace neverd::emulation
