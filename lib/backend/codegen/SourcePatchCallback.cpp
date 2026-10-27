//===- SourcePatchCallback.cpp - Original entry relocation ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Relocate admitted native entry instructions without changing ABI state.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/SourcePatchCallback.h"

#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <capstone/capstone.h>

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "original callback: " + Message);
}

bool plainX86(unsigned ID) {
  switch (ID) {
  case X86_INS_NOP:
  case X86_INS_ENDBR32:
  case X86_INS_ENDBR64:
  case X86_INS_PUSH:
  case X86_INS_POP:
  case X86_INS_MOV:
  case X86_INS_MOVABS:
  case X86_INS_MOVSX:
  case X86_INS_MOVSXD:
  case X86_INS_MOVZX:
  case X86_INS_LEA:
  case X86_INS_ADD:
  case X86_INS_ADC:
  case X86_INS_SUB:
  case X86_INS_SBB:
  case X86_INS_AND:
  case X86_INS_OR:
  case X86_INS_XOR:
  case X86_INS_NOT:
  case X86_INS_NEG:
  case X86_INS_TEST:
  case X86_INS_CMP:
  case X86_INS_INC:
  case X86_INS_DEC:
  case X86_INS_SHL:
  case X86_INS_SHR:
  case X86_INS_SAR:
  case X86_INS_IMUL:
  case X86_INS_MOVAPS:
  case X86_INS_MOVUPS:
  case X86_INS_MOVAPD:
  case X86_INS_MOVUPD:
  case X86_INS_MOVDQA:
  case X86_INS_MOVDQU:
  case X86_INS_MOVSS:
  case X86_INS_MOVSD:
  case X86_INS_PXOR:
  case X86_INS_XORPS:
  case X86_INS_XORPD:
    return true;
  default:
    return false;
  }
}

bool plainARM(unsigned ID) {
  switch (ID) {
  case ARM_INS_ALIAS_NOP:
  case ARM_INS_PUSH:
  case ARM_INS_POP:
  case ARM_INS_MOV:
  case ARM_INS_MOVW:
  case ARM_INS_MOVT:
  case ARM_INS_ADD:
  case ARM_INS_SUB:
  case ARM_INS_AND:
  case ARM_INS_ORR:
  case ARM_INS_EOR:
  case ARM_INS_BIC:
  case ARM_INS_CMP:
  case ARM_INS_CMN:
  case ARM_INS_TST:
  case ARM_INS_LSL:
  case ARM_INS_LSR:
  case ARM_INS_ASR:
  case ARM_INS_LDR:
  case ARM_INS_LDRB:
  case ARM_INS_LDRH:
  case ARM_INS_STR:
  case ARM_INS_STRB:
  case ARM_INS_STRH:
  case ARM_INS_STMDB:
  case ARM_INS_ALIAS_VPUSH:
  case ARM_INS_ALIAS_VPOP:
    return true;
  default:
    return false;
  }
}

bool plainAArch64(unsigned ID) {
  switch (ID) {
  case AARCH64_INS_ALIAS_NOP:
  case AARCH64_INS_ALIAS_BTI:
  case AARCH64_INS_ALIAS_PACIASP:
  case AARCH64_INS_ALIAS_PACIBSP:
  case AARCH64_INS_MOV:
  case AARCH64_INS_MOVK:
  case AARCH64_INS_MOVZ:
  case AARCH64_INS_MOVN:
  case AARCH64_INS_ADD:
  case AARCH64_INS_SUB:
  case AARCH64_INS_AND:
  case AARCH64_INS_ORR:
  case AARCH64_INS_EOR:
  case AARCH64_INS_STP:
  case AARCH64_INS_LDP:
  case AARCH64_INS_STR:
  case AARCH64_INS_STUR:
  case AARCH64_INS_LDR:
  case AARCH64_INS_LDUR:
  case AARCH64_INS_STRB:
  case AARCH64_INS_STRH:
  case AARCH64_INS_LDRB:
  case AARCH64_INS_LDRH:
  case AARCH64_INS_ALIAS_CMP:
  case AARCH64_INS_ALIAS_CMN:
  case AARCH64_INS_ALIAS_TST:
    return true;
  default:
    return false;
  }
}

llvm::Error relocateInstruction(const cs_insn &I, Arch Architecture,
                                InstructionMode Mode, uint64_t NewVA,
                                llvm::MutableArrayRef<uint8_t> Out,
                                uint64_t OriginalVA, uint64_t OverwriteSize) {
  if (!I.detail)
    return fail("decoder omitted instruction details");
  auto Overwrites = [&](uint64_t Address, uint64_t Size) {
    return Address >= OriginalVA ? Address - OriginalVA < OverwriteSize
                                 : OriginalVA - Address < Size;
  };
  if (Architecture == Arch::X64 || Architecture == Arch::X86) {
    if (!plainX86(I.id))
      return fail("unmodeled x86 entry instruction: " +
                  llvm::Twine(I.mnemonic));
    const auto &X = I.detail->x86;
    for (unsigned N = 0; N < X.op_count; ++N) {
      const auto &Op = X.operands[N];
      if (Op.type == X86_OP_MEM &&
          (Op.mem.segment == X86_REG_FS || Op.mem.segment == X86_REG_GS))
        return fail("segmented entry memory requires an explicit thread-state "
                    "contract");
      if (Op.type == X86_OP_REG &&
          (Op.reg == X86_REG_EIP || Op.reg == X86_REG_RIP))
        return fail("unmodeled instruction-pointer register use");
      if (Op.type != X86_OP_MEM ||
          (Op.mem.base != X86_REG_RIP && Op.mem.base != X86_REG_EIP))
        continue;
      const auto &E = X.encoding;
      if (E.disp_size != 4 || !rangeInBounds(E.disp_offset, 4, I.size))
        return fail("PC-relative x86 field has no exact encoding");
      if (Op.mem.base == X86_REG_EIP) {
        // Address-size override truncates to 32 bits before segment addition.
        const uint32_t Target = uint32_t(I.address + I.size + Op.mem.disp);
        if (Overwrites(Target, std::max<unsigned>(1, Op.size)))
          return fail("entry operand refers to overwritten original bytes");
        writeLE<uint32_t>(Out.data() + E.disp_offset,
                          Target - uint32_t(NewVA + I.size));
      } else {
        const int64_t Target = int64_t(I.address + I.size) + Op.mem.disp;
        if (Overwrites(uint64_t(Target), std::max<unsigned>(1, Op.size)))
          return fail("entry operand refers to overwritten original bytes");
        const int64_t Delta = Target - int64_t(NewVA + I.size);
        if (!llvm::isInt<32>(Delta))
          return fail("RIP-relative entry operand exceeds signed 32-bit reach");
        writeLE<uint32_t>(Out.data() + E.disp_offset, uint32_t(Delta));
      }
    }
    return llvm::Error::success();
  }
  if (Architecture == Arch::AArch64) {
    uint32_t Word = readLE<uint32_t>(Out.data());
    if ((Word & 0x1f000000) == 0x10000000) { // ADR / ADRP
      const bool Page = (Word >> 31) != 0;
      const int64_t Immediate = llvm::SignExtend64<21>(
          ((Word >> 29) & 3) | (((Word >> 5) & 0x7ffff) << 2));
      const int64_t Target =
          int64_t(Page ? I.address & ~uint64_t(4095) : I.address) +
          Immediate * (Page ? 4096 : 1);
      const int64_t Delta =
          Target - int64_t(Page ? NewVA & ~uint64_t(4095) : NewVA);
      if (!Page && Overwrites(uint64_t(Target), 1))
        return fail("ADR refers to overwritten original bytes");
      const int64_t Encoded = Page ? Delta / 4096 : Delta;
      if (!llvm::isInt<21>(Encoded))
        return fail("ADR/ADRP entry operand exceeds its architectural reach");
      Word = (Word & ~uint32_t(0x60ffffe0)) | ((uint32_t(Encoded) & 3) << 29) |
             (((uint32_t(Encoded) >> 2) & 0x7ffff) << 5);
      writeLE<uint32_t>(Out.data(), Word);
      return llvm::Error::success();
    }
    if ((Word & 0x3b000000) == 0x18000000) { // Literal loads / PRFM
      const int64_t Target = int64_t(I.address) +
                             llvm::SignExtend64<19>((Word >> 5) & 0x7ffff) * 4;
      if (Overwrites(uint64_t(Target), 16))
        return fail("literal load overlaps overwritten original bytes");
      const int64_t Delta = Target - int64_t(NewVA);
      if ((Delta & 3) || !llvm::isInt<21>(Delta))
        return fail("AArch64 literal exceeds its architectural reach");
      Word = (Word & ~uint32_t(0x00ffffe0)) |
             ((uint32_t(Delta / 4) & 0x7ffff) << 5);
      writeLE<uint32_t>(Out.data(), Word);
      return llvm::Error::success();
    }
    if (!plainAArch64(I.id) && !(I.is_alias && plainAArch64(I.alias_id)))
      return fail("unmodeled AArch64 entry instruction: " +
                  llvm::Twine(I.mnemonic));
    return llvm::Error::success();
  }
  if (!plainARM(I.id) && !(I.is_alias && plainARM(I.alias_id)))
    return fail("unmodeled ARM entry instruction: " + llvm::Twine(I.mnemonic));
  const auto &A = I.detail->arm;
  for (unsigned N = 0; N < A.op_count; ++N) {
    const auto &Op = A.operands[N];
    if (Op.type == ARM_OP_REG && Op.reg == ARM_REG_PC)
      return fail("unmodeled ARM PC-register operand");
    if (Op.type != ARM_OP_MEM)
      continue;
    if (Op.mem.index == ARM_REG_PC)
      return fail("unmodeled ARM PC-register index");
    if (Op.mem.base != ARM_REG_PC)
      continue;
    // Literal loads retain their original storage (including mutable slots).
    // Copying a literal's current value would silently lose that identity.
    if (I.id != ARM_INS_LDR || Op.mem.index != ARM_REG_INVALID ||
        I.detail->writeback)
      return fail("unmodeled ARM PC-relative memory operand");
    const uint64_t OldPC = Mode == InstructionMode::Thumb
                               ? (I.address + 4) & ~uint64_t(3)
                               : I.address + 8;
    const uint64_t NewPC =
        Mode == InstructionMode::Thumb ? (NewVA + 4) & ~uint64_t(3) : NewVA + 8;
    if (Overwrites(OldPC + Op.mem.disp, 4))
      return fail("literal load overlaps overwritten original bytes");
    const int64_t Delta = int64_t(OldPC) + Op.mem.disp - int64_t(NewPC);
    if (Mode == InstructionMode::ARM) {
      uint32_t Word = readLE<uint32_t>(Out.data());
      if ((Word & 0x0f7f0000) != 0x051f0000 || Delta < -4095 || Delta > 4095)
        return fail("ARM literal exceeds its architectural reach");
      Word = (Word & ~uint32_t(0x00800fff)) | (Delta >= 0 ? 0x00800000 : 0) |
             uint32_t(Delta < 0 ? -Delta : Delta);
      writeLE<uint32_t>(Out.data(), Word);
    } else if (I.size == 2) {
      uint16_t Word = readLE<uint16_t>(Out.data());
      if ((Word & 0xf800) != 0x4800 || Delta < 0 || Delta > 1020 || (Delta & 3))
        return fail("Thumb literal exceeds its architectural reach");
      writeLE<uint16_t>(Out.data(), (Word & 0xff00) | uint16_t(Delta / 4));
    } else {
      uint16_t High = readLE<uint16_t>(Out.data());
      uint16_t Low = readLE<uint16_t>(Out.data() + 2);
      if ((High & 0xff7f) != 0xf85f || Delta < -4095 || Delta > 4095)
        return fail("Thumb-2 literal exceeds its architectural reach");
      High = (High & ~uint16_t(0x80)) | (Delta >= 0 ? 0x80 : 0);
      Low = (Low & 0xf000) | uint16_t(Delta < 0 ? -Delta : Delta);
      writeLE<uint16_t>(Out.data(), High);
      writeLE<uint16_t>(Out.data() + 2, Low);
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<SourceCallbackPrefix>
relocateSourceCallbackPrefix(Arch Architecture, InstructionMode Mode,
                             llvm::ArrayRef<uint8_t> Original, va_t OriginalVA,
                             va_t CallbackVA) {
  const auto Target = getTargetCodegenInfo(Architecture, Mode);
  const uint64_t Minimum = Target.sourceTrampolineSize();
  if ((Architecture == Arch::ARM
           ? (Mode != InstructionMode::ARM && Mode != InstructionMode::Thumb)
           : Mode != InstructionMode::Default) ||
      !Minimum || !OriginalVA || !CallbackVA || OriginalVA == CallbackVA ||
      OriginalVA > uint64_t(INT64_MAX) / 2 - 128 ||
      CallbackVA > uint64_t(INT64_MAX) / 2 - 128 ||
      (Architecture == Arch::ARM &&
       (OriginalVA > UINT32_MAX - 128 || CallbackVA > UINT32_MAX - 128)) ||
      ((Architecture == Arch::ARM || Architecture == Arch::AArch64) &&
       ((OriginalVA | CallbackVA) &
        (Mode == InstructionMode::Thumb ? uint64_t(1) : uint64_t(3)))))
    return fail("invalid native callback placement");
  cs_arch CsArch;
  cs_mode CsMode;
  switch (Architecture) {
  case Arch::X86:
  case Arch::X64:
    CsArch = CS_ARCH_X86;
    CsMode = Architecture == Arch::X64 ? CS_MODE_64 : CS_MODE_32;
    break;
  case Arch::ARM:
    CsArch = CS_ARCH_ARM;
    CsMode = static_cast<cs_mode>(
        (Mode == InstructionMode::Thumb ? CS_MODE_THUMB : CS_MODE_ARM) |
        CS_MODE_V8);
    break;
  case Arch::AArch64:
    CsArch = CS_ARCH_AARCH64;
    CsMode = CS_MODE_ARM;
    break;
  default:
    return fail("unsupported native callback architecture");
  }
  csh Handle = 0;
  if (cs_open(CsArch, CsMode, &Handle) != CS_ERR_OK)
    return fail("cannot initialize instruction decoder");
  auto Close = llvm::scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return fail("cannot enable instruction operand decoding");
  cs_insn *Insn = cs_malloc(Handle);
  if (!Insn)
    return fail("cannot allocate instruction decoder");
  auto Free = llvm::scope_exit([&] { cs_free(Insn, 1); });
  SourceCallbackPrefix Result;
  Result.OriginalVA = OriginalVA;
  Result.CallbackVA = CallbackVA;
  Result.OverwriteSize = Minimum;
  Result.Mode = Mode;
  while (Result.Bytes.size() < Minimum) {
    const size_t Offset = Result.Bytes.size();
    if (Offset >= Original.size())
      return fail("entry ends before complete trampoline instructions");
    const uint8_t *Code = Original.data() + Offset;
    size_t Size = std::min<size_t>(16, Original.size() - Offset);
    uint64_t Address = OriginalVA + Offset;
    if (!cs_disasm_iter(Handle, &Code, &Size, &Address, Insn) || !Insn->size)
      return fail("entry contains an undecodable instruction");
    for (unsigned G : {CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET, CS_GRP_INT,
                       CS_GRP_IRET, CS_GRP_PRIVILEGE})
      if (cs_insn_group(Handle, Insn, G))
        return fail("entry prefix contains a control transfer or trap");
    Result.Bytes.insert(Result.Bytes.end(), Original.begin() + Offset,
                        Original.begin() + Offset + Insn->size);
    if (auto Error =
            relocateInstruction(*Insn, Architecture, Mode, CallbackVA + Offset,
                                llvm::MutableArrayRef<uint8_t>(Result.Bytes)
                                    .slice(Offset, Insn->size),
                                OriginalVA, Minimum))
      return Error;
    Result.Instructions.push_back({uint32_t(Offset), Insn->size});
  }
  Result.CopiedSize = Result.Bytes.size();
  Result.Bytes.resize(Result.CopiedSize + Minimum);
  if (!Target.writeSourceTrampoline(Result.Bytes, Result.CopiedSize,
                                    OriginalVA + Result.CopiedSize,
                                    CallbackVA + Result.CopiedSize, Minimum))
    return fail("callback continuation exceeds register-neutral branch reach");
  return Result;
}

} // namespace neverd
