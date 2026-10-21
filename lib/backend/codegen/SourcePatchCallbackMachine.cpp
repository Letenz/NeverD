//===- SourcePatchCallbackMachine.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Derive frame rules from relocated entries.
//===----------------------------------------------------------------------===//

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/loader/LanguageEHDwarf.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"

#include <capstone/capstone.h>
#include <map>
#include <set>

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &S) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "callback machine frame: " + S);
}
struct EntryFrame {
  DwarfCIE CIE;
  DwarfFDE FDE;
  // Inverse stack operations in forward instruction order, used by EHABI.
  std::vector<std::vector<uint8_t>> ARMUndo;
};
llvm::Expected<EntryFrame> derive(const SourceCallbackPrefix &P, Arch A) {
  const bool X86 = A == Arch::X86 || A == Arch::X64;
  const unsigned Width = A == Arch::X64 || A == Arch::AArch64 ? 8 : 4;
  EntryFrame R;
  R.CIE.CodeAlignmentFactor = 1;
  R.CIE.DataAlignmentFactor = -int64_t(Width);
  R.CIE.ReturnAddressRegister = A == Arch::X86   ? 8
                                : A == Arch::X64 ? 16
                                : A == Arch::ARM ? 14
                                                 : 30;
  const unsigned SP = A == Arch::X86   ? 4
                      : A == Arch::X64 ? 7
                      : A == Arch::ARM ? 13
                                       : 31;
  R.FDE.InitialLocation = P.OriginalVA;
  R.FDE.AddressRange = P.CopiedSize;
  auto Rule = [](std::vector<CFIInstruction> &Rules, CFIOpKind K, uint64_t At,
                 unsigned Reg, int64_t Offset) {
    CFIInstruction I;
    I.Kind = K;
    I.CodeOffset = At;
    I.Register = Reg;
    I.Offset = Offset;
    Rules.push_back(std::move(I));
  };
  int64_t Depth = X86 ? Width : 0;
  Rule(R.CIE.InitialInstructions, CFIOpKind::DefCFA, 0, SP, Depth);
  if (X86)
    Rule(R.CIE.InitialInstructions, CFIOpKind::Offset, 0,
         R.CIE.ReturnAddressRegister, -int64_t(Width));
  csh H = 0;
  const cs_arch CA = X86              ? CS_ARCH_X86
                     : A == Arch::ARM ? CS_ARCH_ARM
                                      : CS_ARCH_AARCH64;
  const cs_mode CM = A == Arch::X86   ? CS_MODE_32
                     : A == Arch::X64 ? CS_MODE_64
                     : P.Mode == InstructionMode::Thumb
                         ? cs_mode(CS_MODE_THUMB | CS_MODE_V8)
                         : CS_MODE_ARM;
  if (cs_open(CA, CM, &H) != CS_ERR_OK)
    return fail("cannot open instruction decoder");
  auto Close = llvm::scope_exit([&] { cs_close(&H); });
  cs_option(H, CS_OPT_DETAIL, CS_OPT_ON);
  cs_insn *I = cs_malloc(H);
  if (!I)
    return fail("cannot allocate instruction decoder");
  auto Free = llvm::scope_exit([&] { cs_free(I, 1); });
  auto RegNumber = [&](unsigned Reg) -> std::optional<unsigned> {
    if (X86) {
      const std::map<unsigned, unsigned> X64 = {
          {X86_REG_RAX, 0},  {X86_REG_RDX, 1},  {X86_REG_RCX, 2},
          {X86_REG_RBX, 3},  {X86_REG_RSI, 4},  {X86_REG_RDI, 5},
          {X86_REG_RBP, 6},  {X86_REG_RSP, 7},  {X86_REG_R8, 8},
          {X86_REG_R9, 9},   {X86_REG_R10, 10}, {X86_REG_R11, 11},
          {X86_REG_R12, 12}, {X86_REG_R13, 13}, {X86_REG_R14, 14},
          {X86_REG_R15, 15}};
      const std::map<unsigned, unsigned> X32 = {
          {X86_REG_EAX, 0}, {X86_REG_ECX, 1}, {X86_REG_EDX, 2},
          {X86_REG_EBX, 3}, {X86_REG_ESP, 4}, {X86_REG_EBP, 5},
          {X86_REG_ESI, 6}, {X86_REG_EDI, 7}};
      const std::map<unsigned, unsigned> Alias = {
          {X86_REG_AL, 0},
          {X86_REG_AH, 0},
          {X86_REG_AX, 0},
          {X86_REG_EAX, 0},
          {X86_REG_BL, 3},
          {X86_REG_BH, 3},
          {X86_REG_BX, 3},
          {X86_REG_EBX, 3},
          {X86_REG_CL, A == Arch::X64 ? 2u : 1u},
          {X86_REG_CH, A == Arch::X64 ? 2u : 1u},
          {X86_REG_CX, A == Arch::X64 ? 2u : 1u},
          {X86_REG_ECX, A == Arch::X64 ? 2u : 1u},
          {X86_REG_DL, A == Arch::X64 ? 1u : 2u},
          {X86_REG_DH, A == Arch::X64 ? 1u : 2u},
          {X86_REG_DX, A == Arch::X64 ? 1u : 2u},
          {X86_REG_EDX, A == Arch::X64 ? 1u : 2u},
          {X86_REG_SP, SP},
          {X86_REG_SPL, SP},
          {X86_REG_ESP, SP},
          {X86_REG_BP, A == Arch::X64 ? 6u : 5u},
          {X86_REG_BPL, A == Arch::X64 ? 6u : 5u},
          {X86_REG_EBP, A == Arch::X64 ? 6u : 5u},
          {X86_REG_SI, A == Arch::X64 ? 4u : 6u},
          {X86_REG_SIL, A == Arch::X64 ? 4u : 6u},
          {X86_REG_ESI, A == Arch::X64 ? 4u : 6u},
          {X86_REG_DI, A == Arch::X64 ? 5u : 7u},
          {X86_REG_DIL, A == Arch::X64 ? 5u : 7u},
          {X86_REG_EDI, A == Arch::X64 ? 5u : 7u}};
      if (auto F = Alias.find(Reg); F != Alias.end())
        return F->second;
      const auto &M = A == Arch::X64 ? X64 : X32;
      auto Found = M.find(Reg);
      if (Found != M.end())
        return Found->second;
    } else if (A == Arch::ARM) {
      if (Reg >= ARM_REG_R0 && Reg <= ARM_REG_R12)
        return Reg - ARM_REG_R0;
      if (Reg == ARM_REG_SP)
        return 13;
      if (Reg == ARM_REG_LR)
        return 14;
    } else {
      if (Reg >= AARCH64_REG_W0 && Reg <= AARCH64_REG_W30)
        return Reg - AARCH64_REG_W0;
      if (Reg == AARCH64_REG_WSP)
        return 31;
      if (Reg >= AARCH64_REG_X0 && Reg <= AARCH64_REG_X28)
        return Reg - AARCH64_REG_X0;
      if (Reg == AARCH64_REG_FP)
        return 29;
      if (Reg == AARCH64_REG_LR)
        return 30;
      if (Reg == AARCH64_REG_SP)
        return 31;
    }
    return std::nullopt;
  };
  const uint8_t *B = P.Bytes.data();
  size_t Left = P.CopiedSize;
  uint64_t PC = P.CallbackVA;
  std::set<unsigned> Saved;
  std::map<unsigned, int64_t> SavedOffsets;
  if (X86)
    SavedOffsets[R.CIE.ReturnAddressRegister] = -int64_t(Width);
  std::optional<int64_t> FPDepth;
  while (Left) {
    if (!cs_disasm_iter(H, &B, &Left, &PC, I) || !I->detail)
      return fail("undecodable original instruction");
    const uint64_t At = PC - P.CallbackVA;
    bool Handled = false;
    auto Allocate = [&](int64_t N) -> llvm::Error {
      if (N <= 0 ||
          N > ::neverd::limits::backend::kMaxSourceCallbackCopyBytes ||
          N % Width)
        return fail("unmodeled stack adjustment");
      Depth += N;
      Rule(R.FDE.Instructions, CFIOpKind::DefCFAOffset, At, SP, Depth);
      if (A == Arch::ARM) {
        std::vector<uint8_t> Undo;
        for (; N > 0;
             N -= std::min<int64_t>(
                 N, ::neverd::limits::backend::
                        kSourcePatchCallbackMachineDeriveAllocateNMinArgument2))
          Undo.push_back(
              std::min<int64_t>(
                  N,
                  ::neverd::limits::backend::
                      kSourcePatchCallbackMachineDeriveAllocateMinArgument2) /
                  4 -
              1);
        R.ARMUndo.push_back(std::move(Undo));
      }
      return llvm::Error::success();
    };
    if (X86) {
      const auto &D = I->detail->x86;
      const unsigned SPR = A == Arch::X64 ? X86_REG_RSP : X86_REG_ESP;
      const unsigned FPR = A == Arch::X64 ? X86_REG_RBP : X86_REG_EBP;
      if (I->id == X86_INS_PUSH && D.op_count == 1 &&
          D.operands[0].type == X86_OP_REG && D.operands[0].size == Width) {
        auto Reg = RegNumber(D.operands[0].reg);
        if (!Reg || *Reg == SP || !Saved.insert(*Reg).second)
          return fail("unmodeled register push");
        if (auto E = Allocate(Width))
          return E;
        Rule(R.FDE.Instructions, CFIOpKind::Offset, At, *Reg, -Depth);
        SavedOffsets[*Reg] = -Depth;
        Handled = true;
      } else if (I->id == X86_INS_SUB && D.op_count == 2 &&
                 D.operands[0].type == X86_OP_REG && D.operands[0].reg == SPR &&
                 D.operands[1].type == X86_OP_IMM) {
        if (auto E = Allocate(D.operands[1].imm))
          return E;
        Handled = true;
      } else if (I->id == X86_INS_MOV && D.op_count == 2 &&
                 D.operands[0].type == X86_OP_REG && D.operands[0].reg == FPR &&
                 D.operands[1].type == X86_OP_REG && D.operands[1].reg == SPR) {
        auto Reg = RegNumber(FPR);
        if (!Saved.count(*Reg))
          return fail("frame register overwritten before saving");
        FPDepth = Depth;
        Handled = true; // Keep CFA expressed through the fixed stack depth.
      }
    } else if (A == Arch::ARM) {
      const auto &D = I->detail->arm;
      const bool PushAlias =
          I->id == ARM_INS_STMDB && I->detail->writeback && D.op_count > 1 &&
          D.operands[0].type == ARM_OP_REG && D.operands[0].reg == ARM_REG_SP;
      if (I->id == ARM_INS_PUSH ||
          (I->is_alias && (I->alias_id == ARM_INS_ALIAS_PUSH ||
                           I->alias_id == ARM_INS_ALIAS_PUSHW)) ||
          PushAlias) {
        uint32_t Mask = 0;
        std::vector<unsigned> Regs;
        const bool HasBase = D.op_count && D.operands[0].type == ARM_OP_REG &&
                             D.operands[0].reg == ARM_REG_SP;
        for (unsigned N = HasBase ? 1 : 0; N < D.op_count; ++N) {
          auto Reg = D.operands[N].type == ARM_OP_REG
                         ? RegNumber(D.operands[N].reg)
                         : std::nullopt;
          if (!Reg || *Reg < 4 || *Reg > 14 || *Reg == 13 ||
              !Saved.insert(*Reg).second)
            return fail("unmodeled ARM push register");
          Mask |= 1u << (*Reg - 4);
          Regs.push_back(*Reg);
        }
        if (!Mask || !std::is_sorted(Regs.begin(), Regs.end()))
          return fail("unordered ARM push");
        Depth += Regs.size() * 4;
        Rule(R.FDE.Instructions, CFIOpKind::DefCFAOffset, At, SP, Depth);
        for (size_t N = 0; N < Regs.size(); ++N) {
          const int64_t Offset = -Depth + int64_t(N * 4);
          Rule(R.FDE.Instructions, CFIOpKind::Offset, At, Regs[N], Offset);
          SavedOffsets[Regs[N]] = Offset;
        }
        R.ARMUndo.push_back({uint8_t(0x80 | (Mask >> 8)), uint8_t(Mask)});
        Handled = true;
      } else if (I->id == ARM_INS_SUB && D.op_count >= 2 &&
                 D.operands[0].type == ARM_OP_REG &&
                 D.operands[0].reg == ARM_REG_SP &&
                 D.operands[D.op_count - 1].type == ARM_OP_IMM &&
                 (D.op_count == 2 || (D.operands[1].type == ARM_OP_REG &&
                                      D.operands[1].reg == ARM_REG_SP))) {
        if (auto E = Allocate(D.operands[D.op_count - 1].imm))
          return E;
        Handled = true;
      } else if (I->id == ARM_INS_MOV && D.op_count == 2 &&
                 D.operands[0].type == ARM_OP_REG &&
                 D.operands[1].type == ARM_OP_REG &&
                 D.operands[1].reg == ARM_REG_SP) {
        auto Reg = RegNumber(D.operands[0].reg);
        if (Reg && Saved.count(*Reg))
          Handled = true;
      }
    } else {
      const auto &D = I->detail->aarch64;
      if (I->id == AARCH64_INS_SUB && D.op_count == 3 &&
          D.operands[0].type == AARCH64_OP_REG &&
          D.operands[0].reg == AARCH64_REG_SP &&
          D.operands[1].type == AARCH64_OP_REG &&
          D.operands[1].reg == AARCH64_REG_SP &&
          D.operands[2].type == AARCH64_OP_IMM && !D.operands[2].shift.value) {
        if (auto E = Allocate(D.operands[2].imm))
          return E;
        Handled = true;
      } else if (I->id == AARCH64_INS_STP && D.op_count == 3 &&
                 D.operands[0].type == AARCH64_OP_REG &&
                 D.operands[1].type == AARCH64_OP_REG &&
                 D.operands[2].type == AARCH64_OP_MEM &&
                 D.operands[2].mem.base == AARCH64_REG_SP &&
                 D.operands[2].mem.index == AARCH64_REG_INVALID) {
        auto R0 = RegNumber(D.operands[0].reg),
             R1 = RegNumber(D.operands[1].reg);
        if (!R0 || !R1 || *R0 == SP || *R1 == SP || *R0 == *R1 ||
            Saved.count(*R0) || Saved.count(*R1))
          return fail("unmodeled AArch64 save pair");
        int64_t Off = D.operands[2].mem.disp;
        if (I->detail->writeback) {
          if (auto E = Allocate(-Off))
            return E;
          Off = 0;
        }
        if (Off < 0 || Off + 16 > Depth || Off % 8)
          return fail("save pair is outside the allocated stack");
        Rule(R.FDE.Instructions, CFIOpKind::Offset, At, *R0, Off - Depth);
        Rule(R.FDE.Instructions, CFIOpKind::Offset, At, *R1, Off + 8 - Depth);
        Saved.insert(*R0);
        Saved.insert(*R1);
        SavedOffsets[*R0] = Off - Depth;
        SavedOffsets[*R1] = Off + 8 - Depth;
        Handled = true;
      }
    }
    if (!Handled) {
      // A compact recipe describes a completed prologue. It cannot be copied
      // to arbitrary prefix PCs. Admit only instructions proved frame-neutral.
      cs_regs Read, Write;
      uint8_t NR = 0, NW = 0;
      if (cs_regs_access(H, I, Read, &NR, Write, &NW) != CS_ERR_OK)
        return fail("register effects unavailable");
      for (unsigned N = 0; N < NW; ++N) {
        const auto Reg = RegNumber(Write[N]);
        const llvm::StringRef Name(cs_reg_name(H, Write[N]));
        if (!Reg && Name != "eflags" && Name != "cpsr" && Name != "apsr" &&
            Name != "nzcv")
          return fail("unmodeled register write in entry: " + Name);
        if (Reg &&
            (*Reg == SP || Saved.count(*Reg) ||
             (X86 ? (A == Arch::X86 ? *Reg >= 3 && *Reg <= 7
                                    : *Reg == 3 || *Reg == 6 || *Reg >= 12)
              : A == Arch::ARM ? *Reg >= 4
                               : *Reg >= 19)))
          return fail("instruction changes an unmodeled frame register: " +
                      llvm::Twine(I->mnemonic));
      }
      // Only nops, comparisons and register-only operations are frame-neutral
      // here. Stores need an explicit saved-slot alias model, so fail closed.
      bool MemoryWrite = false;
      if (X86) {
        const auto &D = I->detail->x86;
        for (unsigned N = 0; N < D.op_count; ++N)
          MemoryWrite |= D.operands[N].type == X86_OP_MEM &&
                         (D.operands[N].access & CS_AC_WRITE);
      } else if (A == Arch::ARM) {
        const auto &D = I->detail->arm;
        for (unsigned N = 0; N < D.op_count; ++N)
          MemoryWrite |= D.operands[N].type == ARM_OP_MEM &&
                         (D.operands[N].access & CS_AC_WRITE);
      } else {
        const auto &D = I->detail->aarch64;
        for (unsigned N = 0; N < D.op_count; ++N)
          MemoryWrite |= D.operands[N].type == AARCH64_OP_MEM &&
                         (D.operands[N].access & CS_AC_WRITE);
      }
      if (MemoryWrite) {
        std::optional<int64_t> Address;
        unsigned Size = 0;
        if (X86 && I->id == X86_INS_MOV) {
          const auto &O = I->detail->x86.operands[0];
          if (O.type == X86_OP_MEM && O.mem.index == X86_REG_INVALID) {
            const unsigned SPR = A == Arch::X64 ? X86_REG_RSP : X86_REG_ESP;
            const unsigned FPR = A == Arch::X64 ? X86_REG_RBP : X86_REG_EBP;
            if (O.mem.base == SPR)
              Address = O.mem.disp - Depth;
            if (O.mem.base == FPR && FPDepth)
              Address = O.mem.disp - *FPDepth;
            Size = O.size;
          }
        } else if (A == Arch::ARM && I->id == ARM_INS_STR &&
                   !I->detail->writeback) {
          const auto &D = I->detail->arm;
          if (D.op_count == 2 && D.operands[1].type == ARM_OP_MEM &&
              D.operands[1].mem.base == ARM_REG_SP &&
              D.operands[1].mem.index == ARM_REG_INVALID) {
            Address = D.operands[1].mem.disp - Depth;
            Size = 4;
          }
        }
        if (!Address || !Size ||
            *Address < -Depth - (A == Arch::X64 ? 128 : 0) ||
            *Address + Size > 0)
          return fail("store requires saved-slot alias analysis");
        for (const auto &[Reg, Slot] : SavedOffsets)
          if (*Address < Slot + Width && Slot < *Address + Size)
            return fail("store overwrites an entry register save");
      }
    }
  }
  return R;
}
} // namespace
llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackMachineDwarf(const SourceCallbackPrefix &P, Arch A,
                                 uint64_t FrameVA) {
  auto R = derive(P, A);
  if (!R)
    return R.takeError();
  return encodeSourceCallbackDwarfFrame(P, R->CIE, R->FDE, FrameVA);
}
llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackEHABI(const SourceCallbackPrefix &P) {
  auto R = derive(P, Arch::ARM);
  if (!R)
    return R.takeError();
  std::vector<uint8_t> Ops;
  for (auto I = R->ARMUndo.rbegin(); I != R->ARMUndo.rend(); ++I)
    Ops.insert(Ops.end(), I->begin(), I->end());
  Ops.push_back(0xb0);
  // Personality 1 carries two opcode bytes followed by up to 255 words.
  const unsigned Extra = Ops.size() > 2 ? (Ops.size() - 2 + 3) / 4 : 0;
  if (Extra > 255)
    return fail("EHABI entry exceeds its bounded opcode extent");
  Ops.resize(2 + Extra * 4, 0xb0);
  std::vector<uint8_t> Bytes(4 + Extra * 4);
  Bytes[3] = 0x81;
  Bytes[2] = Extra;
  Bytes[1] = Ops[0];
  Bytes[0] = Ops[1];
  for (unsigned N = 2; N < Ops.size(); ++N)
    Bytes[4 + (N - 2) / 4 * 4 + 3 - (N - 2) % 4] = Ops[N];
  return Bytes;
}
} // namespace neverd
