//===- X64FPState.h - Physical x87 registers and architectural metadata
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_X64FPSTATE_H
#define NEVERD_EMULATION_ARCH_X64FPSTATE_H
#include "neverd/emulation/Registers.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

namespace neverd::emulation {
namespace x64::fp {
#define NEVERD_X64_FP_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#include "X64FPState.def"
#undef NEVERD_X64_FP_VALUE
} // namespace x64::fp

/// Registers are physical FP0..FP7, with only 16 significant high-word bits.
/// Logical ST(i) is FP[(TOP+i)%8]. Tag is the physical abridged nonempty mask;
/// full tag classifications derive from the retained 80-bit payloads.
/// Last-operation metadata records observed state. Native save/restore may
/// clear inactive exception metadata according to the host processor's rules.
struct X64FPState {
  uint16_t Control = x64::fp::InitialControl;
  uint16_t Status = 0, Opcode = 0;
  uint8_t Tag = 0;
  uint64_t Instruction = 0, Data = 0;
  std::array<RegisterValue, x64::fp::RegisterCount> Registers{};
  bool operator==(const X64FPState &) const = default;
  unsigned top() const {
    return (Status >> x64::fp::TopShift) & x64::fp::TopMask;
  }
  uint16_t fullTag() const;
  void setFullTag(uint16_t Full);
};
struct X64MachineState;
llvm::Error validateX64FPState(const X64FPState &State);
bool isX64FPRegister(CPURegister Register);
llvm::Error validateX64FPRegister(CPURegister Register,
                                  const RegisterValue &Value);
RegisterValue readX64FPRegister(const X64FPState &State, CPURegister Register);
llvm::Error writeX64FPRegister(X64FPState &State, CPURegister Register,
                               const RegisterValue &Value);
/// Standard FXSAVE64 legacy area. The ISA owns TOP rotation, 80-bit lanes and
/// the shared FP/SSE offsets used by the bounded XSAVE codec below.
llvm::Error encodeX64FXState(const X64MachineState &State,
                             llvm::MutableArrayRef<uint8_t> Bytes,
                             uint32_t MXCSRMask = x64::fp::BaselineMXCSRMask);
llvm::Error decodeX64FXState(X64MachineState &State,
                             llvm::ArrayRef<uint8_t> Bytes);
/// Bounded FP/SSE XSAVE profile. Both standard and compacted headers retain
/// the same legacy slots. Declared compacted extension components are accepted
/// only in architectural init state; non-initial extensions are unsupported.
llvm::Error encodeX64XsaveState(
    const X64MachineState &State, llvm::MutableArrayRef<uint8_t> Bytes,
    bool Compacted = false, uint32_t MXCSRMask = x64::fp::BaselineMXCSRMask);
llvm::Error
decodeX64XsaveState(X64MachineState &State, llvm::ArrayRef<uint8_t> Bytes,
                    uint32_t MXCSRMask = x64::fp::BaselineMXCSRMask);
} // namespace neverd::emulation
#endif
