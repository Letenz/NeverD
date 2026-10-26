//===- X86ReferenceBoundary.cpp - Address-field boundaries ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolves a prefix-looking predecessor only through decoded instruction
/// paths from the declared function entry. Generation and matching share this
/// proof; unsupported, unreachable and overlapping streams remain unknown.
///
//===----------------------------------------------------------------------===//

#include "X86ImportReference.h"

#include "capstone/capstone.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd::sigs {
namespace {

#define NEVERD_SIGS_REFERENCE_LIMIT(Name, Value)                               \
  inline constexpr auto Name = Value;
#include "../ReferenceCheckLimits.def"

struct BoundaryDecoder {
  csh Handle = 0;
  cs_insn *Insn = nullptr;

  BoundaryDecoder() {
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &Handle) == CS_ERR_OK &&
        cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) == CS_ERR_OK)
      Insn = cs_malloc(Handle);
  }

  ~BoundaryDecoder() {
    if (Insn)
      cs_free(Insn, 1);
    if (Handle)
      cs_close(&Handle);
  }
};

} // namespace

bool hasDecodedX64AddressBoundary(llvm::ArrayRef<uint8_t> Code, size_t Start) {
  // The field reader has already checked the complete canonical instruction.
  const size_t End = Start + x86::kAddressInstructionSize;
  if (End > MaxReferenceBoundaryBytes)
    return false;
  thread_local BoundaryDecoder Decoder;
  if (!Decoder.Insn)
    return false;

  // An owner is its instruction's offset plus one; zero is unvisited. Walking
  // all reachable paths in this prefix detects entries into another decoded
  // instruction, including a branch into an otherwise harmless operand byte.
  std::vector<size_t> Owners(End, 0);
  std::vector<size_t> Work{0};
  bool Reached = false;
  while (!Work.empty()) {
    size_t At = Work.back();
    Work.pop_back();
    while (At < End) {
      if (Owners[At]) {
        if (Owners[At] != At + 1)
          return false;
        break;
      }
      const uint8_t *Bytes = Code.data() + At;
      size_t Left = End - At;
      uint64_t Address = At;
      if (!cs_disasm_iter(Decoder.Handle, &Bytes, &Left, &Address,
                          Decoder.Insn))
        return false;
      const cs_insn &Insn = *Decoder.Insn;
      const size_t Next = At + Insn.size;
      for (size_t I = At; I < Next; ++I) {
        if (Owners[I])
          return false;
        Owners[I] = At + 1;
      }
      if (At == Start) {
        if (Insn.id != X86_INS_LEA || Insn.size != x86::kAddressInstructionSize)
          return false;
        Reached = true;
      }
      auto InGroup = [&](uint8_t Group) {
        return cs_insn_group(Decoder.Handle, &Insn, Group);
      };
      if (InGroup(CS_GRP_BRANCH_RELATIVE) && !InGroup(CS_GRP_JUMP) &&
          !InGroup(CS_GRP_CALL))
        return false;
      if (InGroup(CS_GRP_JUMP)) {
        const cs_x86 &X86 = Insn.detail->x86;
        if (X86.op_count != 1 || X86.operands[0].type != X86_OP_IMM)
          return false;
        const uint64_t Target = X86.operands[0].imm;
        if (Target < End)
          Work.push_back(static_cast<size_t>(Target));
        if (Insn.id == X86_INS_JMP || Insn.id == X86_INS_LJMP)
          break;
      }
      if (InGroup(CS_GRP_RET) || InGroup(CS_GRP_IRET) || InGroup(CS_GRP_INT) ||
          InGroup(CS_GRP_PRIVILEGE) || Insn.id == X86_INS_HLT ||
          Insn.id == X86_INS_UD0 || Insn.id == X86_INS_UD1 ||
          Insn.id == X86_INS_UD2 || Insn.id == X86_INS_XABORT)
        break;
      At = Next;
    }
  }
  return Reached;
}

} // namespace neverd::sigs
