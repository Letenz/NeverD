//===- NativeOperands.cpp - Typed decoded native operands ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Typed decoded native operands.
//===----------------------------------------------------------------------===//

#include "neverd/decode/NativeOperands.h"

#include <map>

namespace neverd {
NativeOperands describeNativeOperands(const Decoder &D, const DecodedInsn &I,
                                      Arch A, bool IncludeRegisters) {
  NativeOperands Result;
  if (!I.Raw || !I.Raw->detail)
    return Result;
  auto Immediate = [&](unsigned Index, int64_t Value, unsigned Bits) {
    if (!Bits || Bits > 64)
      return;
    const uint64_t Mask = Bits == 64 ? UINT64_MAX : (uint64_t(1) << Bits) - 1;
    Result.Immediates.push_back(
        {Index, static_cast<uint64_t>(Value) & Mask, Bits});
  };
  if (A == Arch::X64 || A == Arch::X86) {
    const auto &Detail = I.Raw->detail->x86;
    for (unsigned N = 0; N < Detail.op_count; ++N)
      if (Detail.operands[N].type == X86_OP_IMM)
        Immediate(N, Detail.operands[N].imm, Detail.operands[N].size * 8);
  } else if (A == Arch::AArch64) {
    const auto &Detail = I.Raw->detail->aarch64;
    unsigned Bits = 64;
    for (unsigned N = 0; N < Detail.op_count; ++N)
      if (Detail.operands[N].type == AARCH64_OP_REG) {
        const auto *Name = cs_reg_name(D.getHandle(), Detail.operands[N].reg);
        if (Name && Name[0] == 'w')
          Bits = 32;
        break;
      }
    for (unsigned N = 0; N < Detail.op_count; ++N)
      if (Detail.operands[N].type == AARCH64_OP_IMM)
        Immediate(N, Detail.operands[N].imm, Bits);
  } else if (A == Arch::ARM) {
    const auto &Detail = I.Raw->detail->arm;
    for (unsigned N = 0; N < Detail.op_count; ++N)
      if (Detail.operands[N].type == ARM_OP_IMM)
        Immediate(N, Detail.operands[N].imm, 32);
  }
  cs_regs Reads{}, Writes{};
  uint8_t ReadCount = 0, WriteCount = 0;
  std::map<std::string, NativeRegister> Registers;
  if (IncludeRegisters &&
      cs_regs_access(D.getHandle(), I.Raw, Reads, &ReadCount, Writes,
                     &WriteCount) == CS_ERR_OK) {
    for (unsigned N = 0; N < ReadCount + WriteCount; ++N) {
      const auto *Name = cs_reg_name(
          D.getHandle(), N < ReadCount ? Reads[N] : Writes[N - ReadCount]);
      if (!Name || !*Name)
        continue;
      auto &R = Registers[Name];
      R.Name = Name;
      if (N < ReadCount)
        R.Read = true;
      else
        R.Write = true;
    }
  }
  for (auto &[Name, R] : Registers)
    Result.Registers.push_back(std::move(R));
  return Result;
}
} // namespace neverd
