//===- IntegerABI.h - Explicit scalar guest call boundaries ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_INTEGERABI_H
#define NEVERD_EMULATION_INTEGERABI_H

#include "neverd/emulation/Registers.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::emulation {
class ExecutionBackend;

enum class IntegerCallingConvention {
#define NEVERD_INTEGER_ABI(Name, ...) Name,
#include "neverd/emulation/IntegerABI.def"
#undef NEVERD_INTEGER_ABI
};

struct IntegerABIInfo {
  GuestArchitecture Architecture;
  llvm::ArrayRef<CPURegister> Arguments;
  CPURegister StackPointer, Result, Link;
  uint64_t WordSize, StackAlignment, ShadowSize, RedZoneSize;

  uint64_t returnAddressSize() const {
    return Link == CPURegister::Invalid ? WordSize : 0;
  }
  uint64_t stackArgumentOffset() const {
    return returnAddressSize() + ShadowSize;
  }
};

struct IntegerCallLayout {
  uint64_t StackPointer;
  uint64_t ReturnStackPointer;
  /// Caller-owned storage at the top of the stack, beyond every argument.
  uint64_t PayloadAddress;
};

struct IntegerArgumentLocation {
  /// Invalid denotes a stack slot, otherwise Address is unused.
  CPURegister Register = CPURegister::Invalid;
  uint64_t Address = 0;
};

/// Non-variadic scalar 64-bit calls. Floating-point, aggregates, narrow-value
/// extension and variadic classification require separate typed ABI policies.
/// No guest OS, syscall numbering, exception policy or loader is implied.
class IntegerABI final {
public:
  static llvm::Expected<IntegerABI> get(IntegerCallingConvention Convention);
  const IntegerABIInfo &info() const { return *Info; }

  /// StackTop and payload are aligned to StackAlignment. Preserve the red zone
  /// below entry SP as part of the supplied stack; reject overflow/underflow.
  llvm::Expected<IntegerCallLayout> layoutCall(uint64_t StackBase,
                                               uint64_t StackSize,
                                               uint64_t ArgumentCount,
                                               uint64_t PayloadSize = 0) const;

  /// Preflight the entire frame before effects, install the return address and
  /// parameters, zero unused argument registers, then set SP. Does not set PC.
  /// Predictable layout/access errors leave state unchanged. A transport error
  /// can have partial effects and must be propagated, never retried as success.
  llvm::Expected<IntegerCallLayout>
  prepareCall(ExecutionBackend &CPU, uint64_t StackBase, uint64_t StackSize,
              uint64_t ReturnPC, llvm::ArrayRef<uint64_t> Arguments,
              uint64_t PayloadSize = 0) const;

  llvm::Error validateStackPointer(uint64_t SP) const;
  llvm::Expected<IntegerArgumentLocation>
  argumentLocation(uint64_t SP, uint64_t Index) const;
  llvm::Expected<uint64_t> readArgument(ExecutionBackend &CPU, uint64_t SP,
                                        uint64_t Index) const;
  llvm::Expected<uint64_t> readReturnAddress(ExecutionBackend &CPU,
                                             uint64_t SP) const;
  llvm::Expected<uint64_t> returnStackPointer(uint64_t SP) const;

private:
  explicit IntegerABI(const IntegerABIInfo &Info) : Info(&Info) {}
  llvm::Error validateCPU(const ExecutionBackend &CPU) const;
  const IntegerABIInfo *Info;
};
} // namespace neverd::emulation
#endif
