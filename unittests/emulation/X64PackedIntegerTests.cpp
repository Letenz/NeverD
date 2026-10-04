//===- X64PackedIntegerTests.cpp - SSE2 integer state and RAM effects ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include <stdexcept>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_USER_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "UserExecutionCases.def"
#undef NEVERD_USER_VALUE
#define NEVERD_PACKED_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_PACKED_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_PACKED_OPERATION(Name, Intrinsic, ...)                          \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
#undef NEVERD_PACKED_TEXT
#undef NEVERD_PACKED_VALUE

enum class Operation {
#define NEVERD_PACKED_OPERATION(Name, ...) Name,
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
};
struct Instruction {
  const char *Name;
  Operation Kind;
  llvm::ArrayRef<uint8_t> Bytes;
};
constexpr Instruction Instructions[] = {
#define NEVERD_PACKED_OPERATION(Name, ...) {#Name, Operation::Name, Name},
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
};
struct Input {
  RegisterValue Left, Right;
};
#define NEVERD_PACKED_INPUT(Name, AL, AH, BL, BH)                              \
  constexpr Input Name{{AL, AH}, {BL, BH}};
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_INPUT
struct Result {
  Operation Kind;
  Input Arguments;
  RegisterValue Value;
};
constexpr Result Results[] = {
#define NEVERD_PACKED_RESULT(Name, Input, Low, High)                           \
  {Operation::Name, Input, {Low, High}},
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_RESULT
};
struct Parameter {
  const char *Name;
  ExecutionBackendKind Backend;
  bool User;
};
void PrintTo(const Parameter &P, std::ostream *OS) { *OS << P.Name; }
constexpr Parameter Parameters[] = {
#define NEVERD_INTEGER_BACKEND(Name, Backend, User)                            \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64IntegerCases.def"
#undef NEVERD_INTEGER_BACKEND
};

void checkHost(const Result &R) {
#if defined(__x86_64__) || defined(_M_X64)
  const auto Left = _mm_loadu_si128(
      reinterpret_cast<const __m128i *>(R.Arguments.Left.data()));
  const auto Right = _mm_loadu_si128(
      reinterpret_cast<const __m128i *>(R.Arguments.Right.data()));
  __m128i Value{};
  switch (R.Kind) {
#define NEVERD_PACKED_OPERATION(Name, Intrinsic, ...)                          \
  case Operation::Name:                                                        \
    Value = Intrinsic(Left, Right);                                            \
    break;
#include "X64PackedIntegerCases.def"
#undef NEVERD_PACKED_OPERATION
  }
  RegisterValue Actual;
  _mm_storeu_si128(reinterpret_cast<__m128i *>(Actual.data()), Value);
  EXPECT_EQ(Actual, R.Value);
#endif
}

class X64PackedInteger : public testing::TestWithParam<Parameter> {
protected:
  std::unique_ptr<ExecutionBackend> CPU;
  void SetUp() override { reset(); }
  void reset() {
    CPU.reset();
    auto B = createExecutionBackend(GetParam().Backend,
                                    GetParam().User
                                        ? ExecutionContract::CheckedUserX64
                                        : ExecutionContract::CheckedX64,
                                    Limit, GuestArchitecture::X64);
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      auto Reason = llvm::toString(std::move(E));
      if (Unavailable &&
          !requireHvf(GetParam().Backend, GuestArchitecture::X64))
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    CPU = std::move(B->CPU);
    llvm::cantFail(
        CPU->map(Code, PageSize, Read | Write | Execute | UserAccessible));
    llvm::cantFail(CPU->map(Data, PageSize, Read | Write | UserAccessible));
    llvm::cantFail(
        CPU->mapAlias(Alias, Data, PageSize, Read | Write | UserAccessible));
  }
  void seed(const Input &Input, uint64_t Address = Data) {
    llvm::cantFail(CPU->setXmm(0, Input.Left));
    llvm::cantFail(CPU->setXmm(1, Input.Right));
    for (unsigned Index = 2; Index < XmmCount; ++Index)
      llvm::cantFail(
          CPU->setXmm(Index, {SentinelLow + Index, SentinelHigh - Index}));
    llvm::cantFail(CPU->setReg(X64Register::AX, SentinelLow));
    llvm::cantFail(CPU->setReg(X64Register::DX, SentinelHigh));
    llvm::cantFail(CPU->setReg(X64Register::CX, Address));
    llvm::cantFail(CPU->setReg(X64Register::FLAGS, Flags));
    llvm::cantFail(CPU->setReg(X64Register::MXCSR, MXCSR));
    llvm::cantFail(CPU->writeInteger(Data, Input.Right[0], WordBytes));
    llvm::cantFail(
        CPU->writeInteger(Data + WordBytes, Input.Right[1], WordBytes));
  }
  ExecutionExit run(llvm::ArrayRef<uint8_t> Instruction,
                    BackendHooks Hooks = {}) {
    std::vector<uint8_t> Bytes(Instruction.begin(), Instruction.end());
    Bytes.push_back(Nop);
    llvm::cantFail(CPU->write(Code, Bytes));
    Hooks.Instruction = [&](uint64_t PC, uint32_t) {
      if (PC != Code)
        CPU->stop();
    };
    llvm::cantFail(CPU->installHooks(std::move(Hooks)));
    return llvm::cantFail(CPU->runUntilExit(Code, Timeout));
  }
  std::vector<uint8_t> memory(const Instruction &I) {
    std::vector<uint8_t> Bytes(I.Bytes.begin(), I.Bytes.end());
    Bytes.back() = MemoryModRM;
    return Bytes;
  }
  void expectState(const Input &Input, const RegisterValue &Expected,
                   uint64_t PC, uint64_t Address = Data) {
    EXPECT_EQ(llvm::cantFail(CPU->xmm(0)), Expected);
    EXPECT_EQ(llvm::cantFail(CPU->xmm(1)), Input.Right);
    for (unsigned Index = 2; Index < XmmCount; ++Index)
      EXPECT_EQ(llvm::cantFail(CPU->xmm(Index)),
                (RegisterValue{SentinelLow + Index, SentinelHigh - Index}));
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::AX)), SentinelLow);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::DX)), SentinelHigh);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::CX)), Address);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::FLAGS)), Flags);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::MXCSR)), MXCSR);
    EXPECT_EQ(llvm::cantFail(CPU->reg(X64Register::PC)), PC);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias, WordBytes)),
              Input.Right[0]);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Alias + WordBytes, WordBytes)),
              Input.Right[1]);
  }
};

TEST_P(X64PackedInteger, ArithmeticMatchesGoldenVectorsAndHostInstructions) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (const auto &R : Results) {
      if (R.Kind != I.Kind)
        continue;
      checkHost(R);
      for (bool Memory : {false, true}) {
        SCOPED_TRACE(Memory);
        const uint64_t Address = Alias + PageSize - VectorBytes;
        seed(R.Arguments, Address);
        llvm::cantFail(
            CPU->writeInteger(Address, R.Arguments.Right[0], WordBytes));
        llvm::cantFail(CPU->writeInteger(Address + WordBytes,
                                         R.Arguments.Right[1], WordBytes));
        unsigned Reads = 0, Writes = 0;
        BackendHooks Hooks;
        Hooks.Read = [&](uint64_t A, unsigned Size) {
          EXPECT_EQ(A, Address);
          EXPECT_EQ(Size, VectorBytes);
          ++Reads;
        };
        Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
        const auto Bytes =
            Memory ? memory(I)
                   : std::vector<uint8_t>(I.Bytes.begin(), I.Bytes.end());
        const auto Exit = run(Bytes, std::move(Hooks));
        ASSERT_EQ(Exit.Kind, ExecutionExitKind::Stopped) << Exit.Diagnostic;
        EXPECT_EQ(Reads, unsigned(Memory));
        EXPECT_EQ(Writes, 0u);
        expectState(R.Arguments, R.Value, Code + Bytes.size(), Address);
      }
    }
  }
}

TEST_P(X64PackedInteger, SourceObserversStopOrFailBeforeStateChanges) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (bool Throw : {false, true}) {
      reset();
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_TRUE(CPU);
      seed(Mixed);
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t A, unsigned Size) {
        EXPECT_EQ(A, Data);
        EXPECT_EQ(Size, VectorBytes);
        ++Reads;
        if (Throw)
          throw std::runtime_error(ObserverFailure);
        CPU->stop();
      };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
      const auto Exit = run(memory(I), std::move(Hooks));
      EXPECT_EQ(Exit.Kind, Throw ? ExecutionExitKind::BackendFailure
                                 : ExecutionExitKind::Stopped)
          << Exit.Diagnostic;
      EXPECT_EQ(Reads, 1u);
      EXPECT_EQ(Writes, 0u);
      expectState(Mixed, Mixed.Left, Code);
    }
  }
}

TEST_P(X64PackedInteger, SourceFaultsPreserveStateAndResumeAfterRepair) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (bool Unmapped : {false, true}) {
      seed(Mixed);
      if (Unmapped)
        llvm::cantFail(CPU->addressSpace()->unmap(Data, PageSize));
      else
        llvm::cantFail(CPU->protect(Data, PageSize, UserAccessible));
      unsigned Reads = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
      Hooks.RecoverableFault = [](const BackendFault &) { return true; };
      const auto Bytes = memory(I);
      const auto Exit = run(Bytes, std::move(Hooks));
      ASSERT_EQ(Exit.Kind, ExecutionExitKind::RecoverableFault)
          << Exit.Diagnostic;
      ASSERT_TRUE(Exit.Fault);
      EXPECT_EQ(Exit.Fault->Kind, Unmapped ? BackendFaultKind::UnmappedMemory
                                           : BackendFaultKind::Protection);
      EXPECT_EQ(Exit.Fault->Address, Data);
      EXPECT_EQ(Exit.Fault->Size, VectorBytes);
      EXPECT_EQ(Exit.Fault->Access, BackendAccessKind::Read);
      EXPECT_EQ(Reads, 0u);
      expectState(Mixed, Mixed.Left, Code);
      ASSERT_TRUE(CPU->takeRecoverableFault());
      EXPECT_FALSE(CPU->takeRecoverableFault());
      if (Unmapped)
        llvm::cantFail(CPU->mapAlias(Data, Alias, PageSize,
                                     Read | Write | UserAccessible));
      else
        llvm::cantFail(
            CPU->protect(Data, PageSize, Read | Write | UserAccessible));
      const auto Resumed = run(Bytes);
      ASSERT_EQ(Resumed.Kind, ExecutionExitKind::Stopped) << Resumed.Diagnostic;
      const auto &Expected = Results[unsigned(I.Kind) * InputCount];
      ASSERT_EQ(Expected.Kind, I.Kind);
      expectState(Mixed, Expected.Value, Code + Bytes.size());
    }
  }
}

TEST_P(X64PackedInteger, InvalidFormsRejectBeforeObservations) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    for (unsigned Form = 0; Form < InvalidFormCount; ++Form) {
      reset();
      ASSERT_FALSE(HasFatalFailure());
      ASSERT_TRUE(CPU);
      auto Bytes = memory(I);
      uint64_t Address = Data;
      if (Form == MMXForm)
        Bytes.erase(Bytes.begin());
      else if (Form == LockedForm)
        Bytes.insert(Bytes.begin(), Lock);
      else
        ++Address;
      seed(Mixed, Address);
      unsigned Reads = 0, Writes = 0;
      BackendHooks Hooks;
      Hooks.Read = [&](uint64_t, unsigned) { ++Reads; };
      Hooks.Write = [&](uint64_t, unsigned, uint64_t) { ++Writes; };
      const auto Exit = run(Bytes, std::move(Hooks));
      EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
          << Exit.Diagnostic;
      EXPECT_EQ(Reads, 0u);
      EXPECT_EQ(Writes, 0u);
      expectState(Mixed, Mixed.Left, Code, Address);
    }
  }
}

TEST_P(X64PackedInteger, DeviceSourcesRejectWithoutCallingTheDevice) {
  for (const auto &I : Instructions) {
    SCOPED_TRACE(I.Name);
    reset();
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(CPU);
    seed(Mixed, Stack);
    unsigned Calls = 0;
    GuestMMIOCallbacks Device;
    Device.Validate = [&](uint64_t, uint64_t, bool) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.Read = [&](uint64_t, unsigned) -> llvm::Expected<uint64_t> {
      ++Calls;
      return SentinelLow;
    };
    Device.Write = [&](uint64_t, unsigned, uint64_t) {
      ++Calls;
      return llvm::Error::success();
    };
    Device.PrepareRead =
        [&](uint64_t, unsigned) -> llvm::Expected<GuestMMIOPreparedRead> {
      ++Calls;
      return GuestMMIOPreparedRead{SentinelLow, [&] {
                                     ++Calls;
                                     return llvm::Error::success();
                                   }};
    };
    llvm::cantFail(CPU->mapMMIO(Stack, PageSize, std::move(Device)));
    const auto Exit = run(memory(I));
    EXPECT_EQ(Exit.Kind, ExecutionExitKind::UnsupportedOperation)
        << Exit.Diagnostic;
    EXPECT_EQ(Calls, 0u);
    expectState(Mixed, Mixed.Left, Code, Stack);
  }
}

INSTANTIATE_TEST_SUITE_P(ExplicitBackends, X64PackedInteger,
                         testing::ValuesIn(Parameters),
                         [](const auto &Info) { return Info.param.Name; });
} // namespace
} // namespace neverd::emulation
