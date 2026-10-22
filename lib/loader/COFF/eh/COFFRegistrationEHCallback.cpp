//===- COFFRegistrationEHCallback.cpp - PE32 callback boundaries ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Disprove padding guesses inside decoded registration callback instructions.
//===----------------------------------------------------------------------===//
#include "COFFRegistrationEHDetail.h"

#include "neverd/support/ProloguePatterns.h"

#include "llvm/ADT/ScopeExit.h"

#include <capstone/capstone.h>

namespace neverd::coff_loader::registration_detail {

// `cmp r32, N` immediately followed by `ja`, ending at the indirect jump.
// N is the largest in-range index, so the table has N+1 slots. Any other
// indirect jump stays unresolved.
static std::optional<uint32_t>
boundedAbsoluteJumpEntries(const BinaryImage &Img, va_t Compare, va_t Guard,
                           va_t Jump) {
  if (Compare >= Guard || Guard >= Jump || Jump - Compare > 16)
    return std::nullopt;
  const uint8_t *Cmp = Img.readVA(Compare, size_t(Guard - Compare));
  const uint8_t *Above = Img.readVA(Guard, size_t(Jump - Guard));
  if (!Cmp || !Above)
    return std::nullopt;
  uint32_t Imm = 0;
  const size_t CmpLen = size_t(Guard - Compare);
  if (CmpLen == 3 && Cmp[0] == 0x83 && (Cmp[1] & 0xF8) == 0xF8)
    Imm = Cmp[2];
  else if (CmpLen == 6 && Cmp[0] == 0x81 && (Cmp[1] & 0xF8) == 0xF8)
    Imm = readLE<uint32_t>(Cmp + 2);
  else if (CmpLen == 5 && Cmp[0] == 0x3D)
    Imm = readLE<uint32_t>(Cmp + 1);
  else
    return std::nullopt;
  if (Imm == 0 || Imm >= 64)
    return std::nullopt;
  const size_t GuardLen = size_t(Jump - Guard);
  const bool ShortAbove = GuardLen == 2 && Above[0] == 0x77;
  const bool NearAbove = GuardLen == 6 && Above[0] == 0x0F && Above[1] == 0x87;
  if (!ShortAbove && !NearAbove)
    return std::nullopt;
  return Imm + 1;
}

bool disprovesCallbackBoundary(const BinaryImage &Img, va_t Entry,
                               va_t Boundary, size_t &Work,
                               std::optional<va_t> ReturningEntryCall) {
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
        } else if (Jump && X86.op_count == 1 &&
                   X86.operands[0].type == X86_OP_MEM) {
          // MSVC outlines a registration function's cold switch arms past a
          // padding guess. The dispatch is `cmp reg, N; ja; jmp [reg*4+disp]`.
          // Follow only that bounded absolute table. A slot that names this
          // boundary is an independent entry. An unresolved indirect jump
          // still stops the linear walk below.
          const cs_x86_op &MemOp = X86.operands[0];
          const bool AbsoluteScaled =
              MemOp.mem.scale == 4 && MemOp.mem.base == X86_REG_INVALID &&
              MemOp.mem.index != X86_REG_INVALID &&
              MemOp.mem.segment == X86_REG_INVALID &&
              MemOp.mem.disp >= INT32_MIN && MemOp.mem.disp <= INT32_MAX;
          const uint32_t TableDisp = uint32_t(int32_t(MemOp.mem.disp));
          std::optional<va_t> Guard;
          std::optional<va_t> Compare;
          if (AbsoluteScaled && TableDisp != 0) {
            for (const auto &[Start, Stop] : Decoded)
              if (Stop == Begin)
                Guard = Start;
            if (Guard)
              for (const auto &[Start, Stop] : Decoded)
                if (Stop == *Guard)
                  Compare = Start;
          }
          const std::optional<uint32_t> Entries =
              Compare ? boundedAbsoluteJumpEntries(Img, *Compare, *Guard, Begin)
                      : std::nullopt;
          if (Entries && TableDisp <= UINT32_MAX - uint64_t(*Entries) * 4 &&
              *Entries <= limits::kMaxRegistrationEHStateWork - Work) {
            Work += *Entries;
            const uint8_t *Slots = Img.readVA(TableDisp, size_t(*Entries) * 4);
            if (Slots) {
              for (uint32_t I = 0; I < *Entries; ++I) {
                const va_t Target = readLE<uint32_t>(Slots + size_t(I) * 4);
                if (Target == Boundary)
                  return false;
                if (Entry <= Target && Target < Boundary &&
                    isExecutableAddress(Img, Target))
                  Pending.push_back({Target, true});
              }
            }
          }
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
      // `mov eax, [ebp-0x34]` is `8B 45 CC`. The padding scan reads that
      // displacement as int3 and the next prologue-shaped opcode as a
      // function. This instruction falls into that address, so the guess is
      // the operand's successor. A call or jump can still end on a real entry.
      if (Address == Boundary && !Call && !Jump && Boundary != 0)
        if (const uint8_t *Last = Img.readVA(Boundary - 1, 1);
            Last && isTrapPaddingByte(Img.Arch, *Last))
          Disproved = true;
      // Only the immutable entry prefix's checked physical helper supplies
      // this return edge. Ordinary calls, including later calls to that same
      // helper, may leave independent code after a nonreturning invocation.
      CrossedControl |=
          Jump ||
          (Call && (!ReturningEntryCall || Begin != *ReturningEntryCall));
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
