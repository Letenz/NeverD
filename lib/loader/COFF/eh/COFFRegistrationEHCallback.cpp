//===- COFFRegistrationEHCallback.cpp - PE32 callback boundaries ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Disprove padding guesses inside decoded registration callback instructions.
//===----------------------------------------------------------------------===//
#include "COFFRegistrationEHDetail.h"

#include "llvm/ADT/ScopeExit.h"

#include <capstone/capstone.h>

namespace neverd::coff_loader::registration_detail {
bool disprovesCallbackBoundary(const BinaryImage &Img, va_t Entry,
                               va_t Boundary, size_t &Work) {
  if (Entry >= Boundary || Work >= limits::kMaxRegistrationEHStateWork)
    return false;
  const auto *Segment = Img.getSegmentFor(Entry);
  if (!Segment || !Segment->isExecutable())
    return false;
  const auto Size = std::min<uint64_t>(Segment->Size, Segment->Data.size());
  if (Entry - Segment->VA >= Size || Boundary - Segment->VA >= Size)
    return false;
  csh Handle = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_32, &Handle) != CS_ERR_OK)
    return false;
  const auto Close = llvm::make_scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return false;
  auto *Instruction = cs_malloc(Handle);
  if (!Instruction)
    return false;
  const auto Free = llvm::make_scope_exit([&] { cs_free(Instruction, 1); });
  struct PendingBlock {
    va_t Address;
    bool CrossedControl;
  };
  std::vector<PendingBlock> Pending{{Entry, false}};
  std::map<va_t, va_t> Decoded;
  bool Disproved = false;
  // A padding-looking operand can occur after ordinary branches and calls.
  // Follow the bounded instruction graph without entering any call target.
  // Beyond a control transfer, only a guess strictly inside an instruction is
  // disproved; an exact instruction entry retains its independent boundary.
  while (!Pending.empty()) {
    auto [Address, CrossedControl] = Pending.back();
    Pending.pop_back();
    while (Address < Boundary) {
      if (Work >= limits::kMaxRegistrationEHStateWork)
        return false;
      ++Work;
      if (Decoded.count(Address))
        break;
      const uint8_t *Cursor = Segment->Data.data() + Address - Segment->VA;
      size_t Remaining = Size - (Address - Segment->VA);
      const va_t Begin = Address;
      if (!cs_disasm_iter(Handle, &Cursor, &Remaining, &Address, Instruction) ||
          !Instruction->detail ||
          !Img.hasExecutableCodeOwnerRange(Begin, Instruction->size))
        return false;
      auto Next = Decoded.lower_bound(Begin);
      if ((Next != Decoded.end() && Next->first < Address) ||
          (Next != Decoded.begin() && std::prev(Next)->second > Begin))
        return false;
      Decoded.emplace(Begin, Address);
      const bool Call = cs_insn_group(Handle, Instruction, CS_GRP_CALL);
      const bool Jump = cs_insn_group(Handle, Instruction, CS_GRP_JUMP);
      const auto &X86 = Instruction->detail->x86;
      const bool DirectControl = (Call || Jump) && X86.op_count == 1 &&
                                 X86.operands[0].type == X86_OP_IMM;
      if (Call || Jump) {
        if (DirectControl) {
          const va_t Target = uint32_t(X86.operands[0].imm);
          // Conflicting instruction entries cannot authenticate a range, even
          // if another path decodes this guessed entry as an operand byte.
          if (Target == Boundary)
            return false;
          if (Jump && Entry <= Target && Target < Boundary)
            Pending.push_back({Target, true});
        }
      }
      bool Terminator = false;
      for (unsigned Group : {CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET})
        Terminator |= cs_insn_group(Handle, Instruction, Group);
      Terminator |=
          Instruction->id == X86_INS_UD0 || Instruction->id == X86_INS_UD1 ||
          Instruction->id == X86_INS_UD2 || Instruction->id == X86_INS_HLT;
      if (Terminator || (Jump && !DirectControl))
        break;
      if (Address > Boundary)
        Disproved = true;
      CrossedControl |= Call || Jump;
      if (Address == Boundary && !CrossedControl)
        Disproved = true;
      if (Jump &&
          (Instruction->id == X86_INS_JMP || Instruction->id == X86_INS_LJMP))
        break;
    }
  }
  return Disproved;
}
} // namespace neverd::coff_loader::registration_detail
