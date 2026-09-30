//===- X64ExceptionMonitor.cpp - Private native exception boundary --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64ExceptionMonitor.h"

#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"

#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::emulation {
namespace {
using namespace x64::gateway;
constexpr uint8_t Halt[] = {
#define NEVERD_X64_GATEWAY_BYTES(Name, ...) __VA_ARGS__
#include "X64Exceptions.def"
#undef NEVERD_X64_GATEWAY_BYTES
};
static_assert(sizeof(Halt) == HaltBytes);
static_assert(IDTOffset + VectorCount * GateBytes <= x64::PageSize);
static_assert(VectorCount * CodeStride <= x64::PageSize);
} // namespace

uint64_t x64ExceptionMonitorBase(const MemoryProjection &Memory) {
  return llvm::support::endian::read64le(Memory.data() + DataGPA + BaseOffset);
}

llvm::Expected<uint64_t>
initializeX64ExceptionMonitor(MemoryProjection &Memory) {
  using namespace llvm::support::endian;
  using namespace x64::gateway;
  // Caller mappings are the authority. Select a currently unclaimed range,
  // including device pages, rather than silently shadowing a guest mapping.
  uint64_t Base = x64::KernelMin;
  for (;;) {
    if (!x64::canonicalRange(Base, Bytes))
      return diagnostic::error(x64::exceptiontext::Gateway);
    auto I = Memory.mappings().lower_bound(Base);
    if (I == Memory.mappings().end() || I->first - Base >= Bytes)
      break;
    if (x64::PageSize > UINT64_MAX - I->first)
      return diagnostic::error(x64::exceptiontext::Gateway);
    Base = I->first + x64::PageSize;
  }
  auto *Data = Memory.data() + DataGPA;
  std::memset(Data, 0, Bytes);
  write64le(Data + BaseOffset, Base);
  auto Descriptor = [&](unsigned Selector, uint64_t Value) {
    write64le(Data + GDTOffset + (Selector / x64::WordBytes) * x64::WordBytes,
              Value);
  };
  Descriptor(x64::CodeSelector, CodeDescriptor);
  Descriptor(x64::DataSelector, UserDataDescriptor);
  Descriptor(x64::UserCodeSelector, UserCodeDescriptor);
  const uint64_t TSSBase = Base + TSSOffset;
  const uint64_t TSSLimit = TSSBytes - 1;
  const uint64_t TSSLow =
      (TSSLimit & TSSDescriptorLimitLowMask) |
      ((TSSBase & TSSDescriptorBaseLowMask) << TSSDescriptorBaseShift) |
      (TSSDescriptorAttributes << TSSDescriptorAttributesShift) |
      ((TSSLimit & TSSDescriptorLimitHighMask) << TSSDescriptorLimitHighDelta) |
      ((TSSBase & TSSDescriptorBaseHighMask) << TSSDescriptorBaseHighDelta);
  Descriptor(TSSSelector, TSSLow);
  Descriptor(TSSSelector + x64::WordBytes, TSSBase >> OffsetHighShift);
  write64le(Data + TSSOffset + ISTOffset, Base + Bytes);
  write16le(Data + TSSOffset + IOMapOffset, TSSBytes);
  for (unsigned Vector = 0; Vector < VectorCount; ++Vector) {
    if (!x64::isExceptionVector(Vector))
      continue;
    const uint64_t Target = Base + x64::PageSize + Vector * CodeStride;
    auto *Gate = Data + IDTOffset + Vector * GateBytes;
    write16le(Gate, Target);
    write16le(Gate + GateSelectorOffset, x64::CodeSelector);
    Gate[GateISTOffset] = ISTIndex;
    Gate[GateAttributesOffset] = GateAttributes;
    write16le(Gate + GateOffsetMiddle, Target >> OffsetMiddleShift);
    write32le(Gate + GateOffsetHigh, Target >> OffsetHighShift);
    std::memcpy(Memory.data() + CodeGPA + Vector * CodeStride, Halt,
                sizeof(Halt));
  }
  return Base;
}

llvm::Expected<X64Exception>
consumeX64ExceptionMonitor(const MemoryProjection &Memory,
                           X64MachineState &State,
                           const X64MachineState &Before, uint64_t CR2) {
  using namespace x64::gateway;
  const uint64_t Base = x64ExceptionMonitorBase(Memory);
  const uint64_t Code = Base + x64::PageSize;
  const uint64_t PC = State.reg(X64Register::PC);
  if (Base < x64::KernelMin || !x64::canonicalRange(Base, Bytes) ||
      PC < Code + HaltBytes ||
      PC - Code - HaltBytes >= VectorCount * CodeStride ||
      (PC - Code - HaltBytes) % CodeStride)
    return diagnostic::error(x64::exceptiontext::Gateway);
  const unsigned Vector = (PC - Code - HaltBytes) / CodeStride;
  if (!x64::isExceptionVector(Vector))
    return diagnostic::error(x64::exceptiontext::Gateway);
  const bool ErrorCode = x64::exceptionHasError(Vector);
  const uint64_t FrameBytes = (FrameWords + ErrorCode) * x64::WordBytes;
  if (State.reg(X64Register::SP) != Base + Bytes - FrameBytes)
    return diagnostic::error(x64::exceptiontext::Frame);
  const auto *Stack = Memory.data() + StackGPA + x64::PageSize - FrameBytes;
  auto Word = [&](unsigned Index) {
    return llvm::support::endian::read64le(Stack + (Index + ErrorCode) *
                                                       x64::WordBytes);
  };
  const uint64_t SavedPC = Word(FramePC);
  const uint64_t OriginalPC = Before.reg(X64Register::PC);
  const bool FaultPC = SavedPC == OriginalPC;
  const bool TrapPC = x64::exceptionIsTrap(Vector) && SavedPC > OriginalPC &&
                      SavedPC - OriginalPC <= x64::MaxInstructionBytes;
  const uint64_t CS =
      Before.UserMode ? x64::UserCodeSelector : x64::CodeSelector;
  const uint64_t SS =
      Before.UserMode ? x64::UserDataSelector : x64::DataSelector;
  const uint64_t Instrumentation = x64::TrapFlag | x64::ResumeFlag;
  if ((!FaultPC && !TrapPC) || Word(FrameCS) != CS || Word(FrameSS) != SS ||
      Word(FrameSP) != Before.reg(X64Register::SP) ||
      (Word(FrameFlags) & ~Instrumentation) !=
          (Before.reg(X64Register::FLAGS) & ~Instrumentation))
    return diagnostic::error(x64::exceptiontext::Frame);
  State.reg(X64Register::PC) = SavedPC;
  State.reg(X64Register::SP) = Word(FrameSP);
  State.reg(X64Register::FLAGS) =
      (Word(FrameFlags) & ~Instrumentation) |
      (Before.reg(X64Register::FLAGS) & Instrumentation);
  return X64Exception{Vector,
                      ErrorCode ? std::optional<uint64_t>(
                                      llvm::support::endian::read64le(Stack))
                                : std::nullopt,
                      Vector == unsigned(x64::ExceptionVector::PageFault)
                          ? std::optional<uint64_t>(CR2)
                          : std::nullopt};
}
} // namespace neverd::emulation
