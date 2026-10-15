//===- COFFRegistrationEHBodyRange.cpp - Closed PE32 body extents --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exclude trailing callees from a registration body's inferred code range.

#include "COFFRegistrationEHDetail.h"

#include "llvm/ADT/ScopeExit.h"

#include <capstone/capstone.h>

namespace neverd::coff_loader::registration_detail {

std::optional<va_t>
checkedRegistrationRangeEnd(const BinaryImage &Img,
                            const ExceptionAddressRange &Range,
                            const std::vector<va_t> &Roots, size_t &Work) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || !Range.isValid() ||
      Range.End > uint64_t(UINT32_MAX) + 1 || Roots.empty() ||
      Roots.size() > limits::kMaxRegistrationEHStateWork -
                         std::min(Work, limits::kMaxRegistrationEHStateWork))
    return std::nullopt;
  Work += Roots.size();
  csh Handle = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_32, &Handle) != CS_ERR_OK)
    return std::nullopt;
  const auto Close = llvm::scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return std::nullopt;
  auto *Instruction = cs_malloc(Handle);
  if (!Instruction)
    return std::nullopt;
  const auto Free = llvm::scope_exit([&] { cs_free(Instruction, 1); });
  std::vector<va_t> Pending = Roots;
  std::map<va_t, va_t> Decoded;
  va_t End = Range.Begin;
  while (!Pending.empty()) {
    va_t Address = Pending.back();
    Pending.pop_back();
    while (!Decoded.count(Address)) {
      if (Work >= limits::kMaxRegistrationEHStateWork ||
          !Range.contains(Address))
        return std::nullopt;
      ++Work;
      const va_t Begin = Address;
      size_t Remaining = std::min<uint64_t>(15, Range.End - Address);
      const uint8_t *Cursor = Img.readVA(Address, Remaining);
      if (!Cursor ||
          !cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
          !Instruction->detail ||
          !Img.hasExecutableCodeOwnerRange(Begin, Instruction->size))
        return std::nullopt;
      const auto Next = Decoded.lower_bound(Begin);
      if ((Next != Decoded.end() && Next->first < Address) ||
          (Next != Decoded.begin() && std::prev(Next)->second > Begin))
        return std::nullopt;
      Decoded.emplace(Begin, Address);
      End = std::max(End, Address);
      if (cs_insn_group(Handle, Instruction, CS_GRP_RET)) {
        if (Instruction->id != X86_INS_RET)
          return std::nullopt;
        break;
      }
      if (cs_insn_group(Handle, Instruction, CS_GRP_IRET))
        return std::nullopt;
      if (cs_insn_group(Handle, Instruction, CS_GRP_JUMP)) {
        const auto &X86 = Instruction->detail->x86;
        if (X86.op_count != 1 || X86.operands[0].type != X86_OP_IMM)
          return std::nullopt;
        const va_t Target = uint32_t(X86.operands[0].imm);
        if (!Range.contains(Target))
          return std::nullopt;
        Pending.push_back(Target);
        if (Instruction->id == X86_INS_JMP)
          break;
      }
      // Calls retain their ordinary continuation without absorbing the
      // callee. Traps conservatively retain fallthrough too: this boundary
      // proof must not invent a no-return contract or discard resumed code.
    }
  }
  return End;
}

} // namespace neverd::coff_loader::registration_detail
