//===- CheckedX64Stack.cpp - Ordered ordinary-RAM stack footprints ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedX64Backend.h"

namespace neverd::emulation {
llvm::Error
CheckedX64Backend::prepareStack(const cs_insn &I, bool Push,
                                std::vector<Access> &Accesses) const {
  const auto &X = I.detail->x86;
  if (X.op_count != 1)
    return llvm::make_error<UnsupportedExecutionError>();
  const auto &O = X.operands[0];
  const unsigned Size = O.size;
  if (Size != x64::HalfWordBytes && Size != x64::WordBytes)
    return llvm::make_error<UnsupportedExecutionError>();
  const uint64_t SP = CPU.reg(X64Register::SP);
  if (!Push)
    Accesses.push_back({SP, Size, Read, 0});
  if (O.type == X86_OP_MEM) {
    // POP's explicit RSP/ESP base observes the increment. Its implicit stack
    // read always uses the original full RSP, independently of address size.
    const uint64_t Offset =
        !Push && (O.mem.base == X86_REG_RSP || O.mem.base == X86_REG_ESP) ? Size
                                                                          : 0;
    auto Address = operandAddress(I, O, Offset);
    if (!Address)
      return Address.takeError();
    if (Push) {
      Accesses.push_back({*Address, Size, Read, 0});
      Accesses.push_back({SP - Size, Size, Write, 0, std::nullopt, 0, true});
    } else
      Accesses.push_back({*Address, Size, Write, 0, std::nullopt, 0, true});
    // The processor supplies the copied value. RAMTransaction presents it to
    // result observers while original RAM and public CPU state remain intact.
    return llvm::Error::success();
  }
  uint64_t Value = 0;
  if (O.type == X86_OP_REG) {
    auto Register = operandRegister(O.reg);
    if (!Register)
      return Register.takeError();
    Value = *Register;
  } else if (Push && O.type == X86_OP_IMM)
    Value = O.imm;
  else
    return llvm::make_error<UnsupportedExecutionError>();
  if (Push)
    Accesses.push_back({SP - Size, Size, Write, Value});
  return llvm::Error::success();
}
} // namespace neverd::emulation
