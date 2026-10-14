//===- COFFRegistrationEHCallbackRange.cpp - Closed PE32 callback ranges --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "llvm/ADT/ScopeExit.h"

#include <capstone/capstone.h>

namespace neverd::coff_loader::registration_detail {
std::optional<va_t> checkedCallbackRangeEnd(const BinaryImage &Img, va_t Entry,
                                            va_t Boundary, va_t Limit,
                                            size_t &Work) {
  const auto *Segment = Img.getSegmentFor(Entry);
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || !Segment ||
      !Segment->isExecutable() || Entry >= Boundary || Boundary >= Limit)
    return std::nullopt;
  const uint64_t Size = std::min<uint64_t>(Segment->Size, Segment->Data.size());
  if (Size > InvalidVA - Segment->VA || Limit > Segment->VA + Size)
    return std::nullopt;
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
  std::vector<va_t> Pending{Entry};
  std::map<va_t, va_t> Decoded;
  std::set<va_t> Branches, Calls;
  va_t End = Entry;
  bool Returns = false;
  while (!Pending.empty()) {
    va_t Address = Pending.back();
    Pending.pop_back();
    while (!Decoded.count(Address)) {
      if (Work >= limits::kMaxRegistrationEHStateWork || Address < Entry ||
          Address >= Limit)
        return std::nullopt;
      ++Work;
      const va_t Begin = Address;
      const uint8_t *Cursor = Segment->Data.data() + Address - Segment->VA;
      size_t Remaining = Limit - Address;
      if (!cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
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
        Returns = true;
        break;
      }
      if (cs_insn_group(Handle, Instruction, CS_GRP_INT) ||
          cs_insn_group(Handle, Instruction, CS_GRP_IRET) ||
          Instruction->id == X86_INS_UD0 || Instruction->id == X86_INS_UD1 ||
          Instruction->id == X86_INS_UD2 || Instruction->id == X86_INS_HLT)
        return std::nullopt;
      const bool Jump = cs_insn_group(Handle, Instruction, CS_GRP_JUMP);
      const bool Call = cs_insn_group(Handle, Instruction, CS_GRP_CALL);
      const auto &X86 = Instruction->detail->x86;
      const bool Direct =
          X86.op_count == 1 && X86.operands[0].type == X86_OP_IMM;
      if (Call && Direct)
        Calls.insert(uint32_t(X86.operands[0].imm));
      if (Jump) {
        if (!Direct)
          return std::nullopt;
        const va_t Target = uint32_t(X86.operands[0].imm);
        if (Target < Entry || Target >= Limit)
          return std::nullopt;
        Branches.insert(Target);
        Pending.push_back(Target);
        if (Instruction->id == X86_INS_JMP || Instruction->id == X86_INS_LJMP)
          break;
      }
    }
  }
  // An exact entry after a call remains independent unless the table-owned
  // callback's closed graph also reaches it after an ordinary branch.
  // A call target in the proposed range still names an independent callee.
  if (!Returns || !Decoded.count(Boundary) || Branches.empty() ||
      *Branches.begin() > Boundary || End <= Boundary)
    return std::nullopt;
  for (va_t Target : Calls) {
    if (Work >= limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    ++Work;
    if (Entry <= Target && Target < End)
      return std::nullopt;
  }
  return End;
}
} // namespace neverd::coff_loader::registration_detail
