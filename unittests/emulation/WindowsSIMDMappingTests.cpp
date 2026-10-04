//===- WindowsSIMDMappingTests.cpp - Native-derived Windows SIMD records -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/windows/exception/X64SIMDException.h"
#include "os/windows/process/WindowsProcessExceptions.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/IntegerABI.h"

namespace neverd::emulation {
namespace {
using windows_process::ExceptionDispatcher;
#define NEVERD_WINDOWS_SIMD_MAPPING_VALUE(Name, Value)                         \
  constexpr uint64_t Name = Value;
#include "fixtures/WindowsSIMDStatusCases.def"
#undef NEVERD_WINDOWS_SIMD_MAPPING_VALUE
struct Observation {
  uint32_t Active, MXCSR, Code;
};
constexpr Observation Observations[] = {
#define NEVERD_WINDOWS_SIMD_STATUS(Active, MXCSR, Code) {Active, MXCSR, Code},
#include "fixtures/WindowsSIMDStatusCases.def"
#undef NEVERD_WINDOWS_SIMD_STATUS
};
BackendFault simdFault() {
  BackendFault Fault{BackendFaultKind::Interrupt, FaultPC};
  Fault.Interrupt = SIMDVector;
  return Fault;
}

TEST(WindowsSIMDMapping, NativeObservationsDetermineStatusAndParameters) {
  for (const auto &O : Observations) {
    SCOPED_TRACE(O.Active);
    auto Record = windows_exception::x64SIMDException(O.MXCSR);
    ASSERT_TRUE(Record);
    EXPECT_EQ(Record->Code, O.Code);
    EXPECT_EQ(Record->Parameters,
              (std::array<uint64_t, ParameterCount>{0, O.MXCSR}));
    const auto Fault = simdFault();
    auto Raised =
        ExceptionDispatcher::exception(GuestArchitecture::X64, Fault, O.MXCSR);
    ASSERT_TRUE(Raised);
    EXPECT_EQ(Raised->Code, O.Code);
    EXPECT_EQ(Raised->Address, FaultPC);
    EXPECT_EQ(Raised->Flags, 0u);
    EXPECT_EQ(Raised->Arguments, (std::vector<uint64_t>{0, O.MXCSR}));
    EXPECT_TRUE(ExceptionDispatcher::recoverable(GuestArchitecture::X64, Fault,
                                                 O.MXCSR));
  }
}

TEST(WindowsSIMDMapping, RequiresConsistentX64FaultAndControl) {
  const auto Fault = simdFault();
  for (const auto &O : Observations) {
    EXPECT_FALSE(ExceptionDispatcher::exception(GuestArchitecture::AArch64,
                                                Fault, O.MXCSR));
    EXPECT_FALSE(ExceptionDispatcher::exception(GuestArchitecture::X64, Fault));
    for (const auto Control :
         {uint64_t(0), MaskedMXCSR, MaskedMXCSR | StickyStatus,
          O.MXCSR | ReservedMXCSR, O.MXCSR | WideMXCSR}) {
      EXPECT_FALSE(windows_exception::x64SIMDException(Control));
      EXPECT_FALSE(ExceptionDispatcher::recoverable(GuestArchitecture::X64,
                                                    Fault, Control));
    }
#define NEVERD_WINDOWS_SIMD_INVALID_FAULT(Name, Field, Value)                  \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    auto Invalid = Fault;                                                      \
    Invalid.Field = Value;                                                     \
    EXPECT_FALSE(ExceptionDispatcher::exception(GuestArchitecture::X64,        \
                                                Invalid, O.MXCSR));            \
    EXPECT_FALSE(ExceptionDispatcher::recoverable(GuestArchitecture::X64,      \
                                                  Invalid, O.MXCSR));          \
  }
#include "fixtures/WindowsSIMDStatusCases.def"
#undef NEVERD_WINDOWS_SIMD_INVALID_FAULT
  }
}

// Inject only the authenticated fault boundary. The software CPU supplies
// real context/memory/ABI operations; this does not claim its SSE engine traps.
TEST(WindowsSIMDDispatch, RetainedControlReachesRecordsAndMaskedContinuation) {
  auto Selection = createExecutionBackend(ExecutionBackendKind::Unicorn,
                                          ExecutionContract::Software, Limit);
  if (!Selection) {
    auto Error = Selection.takeError();
    const bool Unavailable = Error.isA<BackendUnavailableError>();
    auto Reason = llvm::toString(std::move(Error));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  auto &CPU = *Selection->CPU;
  const uint64_t StackTop = windows_process::value::StackTop;
  const uint64_t StackBase = StackTop - StackPages * PageBytes;
  llvm::cantFail(
      CPU.map(FaultPC, PageBytes, Read | Write | Execute | UserAccessible));
  llvm::cantFail(CPU.map(StackBase, StackPages * PageBytes,
                         Read | Write | UserAccessible));
  auto ABI = llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));
  ExceptionDispatcher Dispatcher(CPU, ABI, StackBase);
  const auto Fault = simdFault();
  EXPECT_FALSE(Dispatcher.accepts(Fault));
  llvm::cantFail(Dispatcher.add(ExceptionDispatcher::HandlerKind::Exception,
                                true, HandlerPC));
  EXPECT_FALSE(Dispatcher.accepts(Fault));
  for (const auto &O : Observations) {
    SCOPED_TRACE(O.Active);
    llvm::cantFail(CPU.setReg(X64Register::PC, FaultPC));
    llvm::cantFail(CPU.setReg(X64Register::SP, StackTop - StackAlignment));
    llvm::cantFail(CPU.setReg(X64Register::MXCSR, O.MXCSR));
    llvm::cantFail(
        CPU.writeRegister(CPURegister::X64V0, {Sentinel, ~Sentinel}));
    ASSERT_TRUE(Dispatcher.accepts(Fault));
    auto Transfer = llvm::cantFail(
        Dispatcher.beginFault(Fault, StackTop - StackAlignment, 0));
    EXPECT_EQ(Transfer.PC, HandlerPC);
    const auto Pointers = llvm::cantFail(CPU.reg(X64Register::CX));
    const auto Record =
        llvm::cantFail(CPU.readInteger(Pointers, sizeof(uint64_t)));
    const auto Context = llvm::cantFail(
        CPU.readInteger(Pointers + sizeof(uint64_t), sizeof(uint64_t)));
    EXPECT_EQ(llvm::cantFail(CPU.readInteger(Record, sizeof(uint32_t))),
              O.Code);
    EXPECT_EQ(llvm::cantFail(CPU.readInteger(Record + RecordFlagsOffset,
                                             sizeof(uint32_t))),
              0u);
    EXPECT_EQ(llvm::cantFail(
                  CPU.readInteger(Record + RecordPCOffset, sizeof(uint64_t))),
              FaultPC);
    EXPECT_EQ(llvm::cantFail(CPU.readInteger(Record + RecordCountOffset,
                                             sizeof(uint32_t))),
              ParameterCount);
    EXPECT_EQ(llvm::cantFail(CPU.readInteger(Record + RecordArgumentsOffset,
                                             sizeof(uint64_t))),
              0u);
    EXPECT_EQ(llvm::cantFail(CPU.readInteger(Record + RecordArgumentsOffset +
                                                 sizeof(uint64_t),
                                             sizeof(uint64_t))),
              O.MXCSR);
    EXPECT_EQ(llvm::cantFail(
                  CPU.readInteger(Context + ContextPCOffset, sizeof(uint64_t))),
              FaultPC);
    for (auto Offset : {ContextMXCSROffset, ContextFXMXCSROffset}) {
      EXPECT_EQ(
          llvm::cantFail(CPU.readInteger(Context + Offset, sizeof(uint32_t))),
          O.MXCSR);
      llvm::cantFail(CPU.writeInteger(Context + Offset, O.MXCSR | MaskedMXCSR,
                                      sizeof(uint32_t)));
    }
    // A handler may clobber CPU state; continuation restores the retained
    // context with both MXCSR copies consistently repaired.
    llvm::cantFail(CPU.writeRegister(CPURegister::X64V0, {0, 0}));
    auto Resumed = llvm::cantFail(Dispatcher.returned(Continuation));
    EXPECT_EQ(Resumed.PC, FaultPC);
    EXPECT_EQ(llvm::cantFail(CPU.reg(X64Register::MXCSR)),
              O.MXCSR | MaskedMXCSR);
    EXPECT_EQ(llvm::cantFail(CPU.readRegister(CPURegister::X64V0)),
              (RegisterValue{Sentinel, ~Sentinel}));
    EXPECT_FALSE(Dispatcher.activeAt(0));
    EXPECT_FALSE(Dispatcher.accepts(Fault));
  }
}
} // namespace
} // namespace neverd::emulation
