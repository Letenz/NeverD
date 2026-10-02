//===- AArch64InstructionEffects.cpp - Checked ARM64 ISA effects ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64InstructionEffects.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::emulation {
namespace {
namespace encoding {
#define NEVERD_AARCH64_ENCODING(Name, Mask, Value)                             \
  bool is##Name(uint32_t Word) { return (Word & Mask) == Value; }
#define NEVERD_AARCH64_FIELD(Name, Value) constexpr unsigned Name = Value;
#include "CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_ENCODING
#undef NEVERD_AARCH64_FIELD
} // namespace encoding
bool scalarRegister(unsigned Register) {
  switch (Register) {
#define NEVERD_AARCH64_OPERAND(Name, Bits)                                     \
  case AARCH64_REG_##Name:                                                     \
    return true;
#include "AArch64OperandRegisters.def"
#undef NEVERD_AARCH64_OPERAND
  default:
    return false;
  }
}
unsigned vectorRegisterWidth(unsigned Register) {
#define NEVERD_AARCH64_VECTOR_VIEW(Name, Bits)                                 \
  if (Register >= AARCH64_REG_##Name##0 && Register <= AARCH64_REG_##Name##31) \
    return Bits;
#include "AArch64OperandRegisters.def"
#undef NEVERD_AARCH64_VECTOR_VIEW
  return 0;
}
bool baselineFeatures(const cs_insn &I) {
  for (unsigned Index = 0; Index < I.detail->groups_count; ++Index) {
    const auto Group = I.detail->groups[Index];
    if (Group >= AARCH64_FEATURE_HASV8_0A &&
        Group != AARCH64_FEATURE_HASV8_0A &&
        Group != AARCH64_FEATURE_HASFPARMV8 && Group != AARCH64_FEATURE_HASNEON)
      return false;
  }
  return true;
}
} // namespace
llvm::Expected<std::vector<AArch64MemoryAccess>>
getAArch64InstructionEffects(const cs_insn &I,
                             const AArch64MachineState &State) {
  using namespace encoding;
  if (I.size != aarch64::InstructionBytes || !I.detail)
    return llvm::make_error<UnsupportedExecutionError>();
  enum InstructionKind {
    Integer,
    Memory,
    System,
    Floating,
    Vector,
    ScalarConversion
  } Kind;
  switch (I.id) {
#define NEVERD_AARCH64_INSTRUCTION(Name, Type)                                 \
  case AARCH64_INS_##Name:                                                     \
    Kind = Type;                                                               \
    break;
#include "CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_INSTRUCTION
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
  const uint32_t Word = llvm::support::endian::read32le(I.bytes);
  if (Kind == System &&
      !((I.id == AARCH64_INS_MRS && isReadThreadPointer(Word)) ||
        (I.id == AARCH64_INS_MSR && isWriteThreadPointer(Word)) ||
        (I.id == AARCH64_INS_MRS &&
         (isReadFPControl(Word) || isReadFPStatus(Word))) ||
        (I.id == AARCH64_INS_MSR &&
         (isWriteFPControl(Word) || isWriteFPStatus(Word)))))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto Source = Word & RegisterMask;
  const bool UsesFloatingState = Kind == Floating || Kind == ScalarConversion;
  if (UsesFloatingState &&
      (State.reg(AArch64Register::FPCR) & ~aarch64::AllowedFPCR))
    return llvm::make_error<UnsupportedExecutionError>();
  if (Kind == System && I.id == AARCH64_INS_MSR &&
      Source != aarch64::GPRCount &&
      ((isWriteFPControl(Word) &&
        (State.Registers[Source] & ~aarch64::AllowedFPCR)) ||
       (isWriteFPStatus(Word) &&
        (State.Registers[Source] & ~aarch64::AllowedFPSR))))
    return llvm::make_error<UnsupportedExecutionError>();
  if (I.id == AARCH64_INS_HINT && !isNOP(Word))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto &A = I.detail->aarch64;
  if (Kind == ScalarConversion) {
    const bool Resize = I.id == AARCH64_INS_FCVT;
    const unsigned FloatIndex =
        I.id == AARCH64_INS_SCVTF || I.id == AARCH64_INS_UCVTF ? 0 : 1;
    // Only scalar GPR <-> binary32/binary64 forms are admitted. Fixed-point
    // immediates, packed vectors and optional half precision remain outside
    // this contract even though Capstone uses the same instruction IDs.
    if (A.op_count != 2 || A.operands[0].type != AARCH64_OP_REG ||
        A.operands[1].type != AARCH64_OP_REG ||
        (!Resize && !scalarRegister(A.operands[1 - FloatIndex].reg)) ||
        A.operands[FloatIndex].vas != AARCH64LAYOUT_INVALID)
      return llvm::make_error<UnsupportedExecutionError>();
    const unsigned Width = vectorRegisterWidth(A.operands[FloatIndex].reg);
    if (Width != 32 && Width != 64)
      return llvm::make_error<UnsupportedExecutionError>();
    if (Resize) {
      const auto &Other = A.operands[1 - FloatIndex];
      const unsigned OtherWidth = vectorRegisterWidth(Other.reg);
      if ((OtherWidth != 32 && OtherWidth != 64) || OtherWidth == Width ||
          Other.vas != AARCH64LAYOUT_INVALID)
        return llvm::make_error<UnsupportedExecutionError>();
    }
  }
  bool HasVector = UsesFloatingState || Kind == Vector;
  for (unsigned N = 0; N < A.op_count; ++N) {
    const auto &O = A.operands[N];
    if (O.type == AARCH64_OP_REG && !scalarRegister(O.reg)) {
      const unsigned Width = vectorRegisterWidth(O.reg);
      if (!Width)
        return llvm::make_error<UnsupportedExecutionError>();
      HasVector = true;
      if (UsesFloatingState &&
          (Width < FP32Bits ||
           (O.vas != AARCH64LAYOUT_INVALID &&
            (unsigned(O.vas) & LayoutElementMask) < FP32Bits)))
        return llvm::make_error<UnsupportedExecutionError>();
    }
    if ((Kind == Integer || Kind == Vector || UsesFloatingState) &&
        O.type != AARCH64_OP_REG && O.type != AARCH64_OP_IMM &&
        !(Kind == Floating && O.type == AARCH64_OP_FP))
      return llvm::make_error<UnsupportedExecutionError>();
  }
  if (HasVector && !baselineFeatures(I))
    return llvm::make_error<UnsupportedExecutionError>();
  std::vector<AArch64MemoryAccess> Accesses;
  auto GPR = [&](unsigned R) {
    return R == aarch64::GPRCount ? uint64_t(0) : State.Registers[R];
  };
  const unsigned Rt = Word & RegisterMask;
  const unsigned Rn = (Word >> BaseShift) & RegisterMask;
  const uint64_t Base =
      Rn == aarch64::GPRCount ? State.reg(AArch64Register::SP) : GPR(Rn);
  if (Kind == Memory) {
    const bool Vector = Word & VectorMemoryBit;
    const auto Operand = [&](unsigned Register) {
      return Vector ? State.Vectors[Register] : RegisterValue{GPR(Register), 0};
    };
    bool Load = Word & LoadBit;
    unsigned Size = 1u << (Word >> SizeShift);
    uint64_t Address = Base;
    bool Writeback = false;
    if (isUnsignedMemory(Word) || isImmediateMemory(Word) ||
        isRegisterMemory(Word)) {
      unsigned Opcode = (Word >> OpcodeShift) & OpcodeMask;
      Load = Vector ? bool(Opcode & VectorLoadBit) : Opcode != 0;
      if (Vector && (Opcode & VectorFullWidthBit)) {
        if (Size != 1)
          return llvm::make_error<UnsupportedExecutionError>();
        Size = VectorBytes;
      }
      // The size=8/opc=2 encoding is prefetch, not a scalar load.
      if (!Vector && Size == aarch64::WordBytes && Opcode > 1)
        return llvm::make_error<UnsupportedExecutionError>();
      if (isUnsignedMemory(Word))
        Address +=
            uint64_t((Word >> UnsignedOffsetShift) & UnsignedOffsetMask) * Size;
      else if (isImmediateMemory(Word)) {
        const unsigned Mode = (Word >> ModeShift) & OpcodeMask;
        Writeback = Mode == 1 || Mode == 3;
        if (Mode != 1)
          Address += llvm::SignExtend64<ImmediateBits>(Word >> ImmediateShift);
      } else {
        const unsigned Rm = (Word >> IndexShift) & RegisterMask;
        uint64_t Offset = GPR(Rm);
        switch ((Word >> ExtendShift) & ExtendMask) {
        case UXTW:
          Offset = uint32_t(Offset);
          break;
        case UXTX:
          break;
        case SXTW:
          Offset = uint64_t(int64_t(int32_t(Offset)));
          break;
        case SXTX:
          break;
        default:
          return llvm::make_error<UnsupportedExecutionError>();
        }
        if (Word & ScaleBit)
          Offset *= Size;
        Address += Offset;
      }
      if (!Vector && Writeback && Rn != aarch64::GPRCount && Rn == Rt)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, Operand(Rt), Size, Load ? Read : Write});
    } else if (isPairMemory(Word)) {
      const unsigned Rt2 = (Word >> SecondShift) & RegisterMask;
      Size = (Word & PairWidthBit) ? aarch64::WordBytes
                                   : aarch64::InstructionBytes;
      if (Vector) {
        const unsigned Opcode = Word >> SizeShift;
        if (Opcode == OpcodeMask)
          return llvm::make_error<UnsupportedExecutionError>();
        Size = aarch64::InstructionBytes << Opcode;
      }
      const unsigned Mode = (Word >> PairModeShift) & OpcodeMask;
      Writeback = Mode == 1 || Mode == 3;
      if (Mode != 1)
        Address += uint64_t(llvm::SignExtend64<PairOffsetBits>(
                       Word >> PairOffsetShift)) *
                   Size;
      if ((Load && Rt == Rt2) ||
          (!Vector && Writeback && Rn != aarch64::GPRCount &&
           (Rn == Rt || Rn == Rt2)))
        return llvm::make_error<UnsupportedExecutionError>();
      if (Size > UINT64_MAX - Address)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, Operand(Rt), Size, Load ? Read : Write});
      Accesses.push_back(
          {Address + Size, Operand(Rt2), Size, Load ? Read : Write});
    } else if (isLiteralMemory(Word)) {
      const unsigned Opcode = Word >> SizeShift;
      if (Opcode == OpcodeMask)
        return llvm::make_error<UnsupportedExecutionError>();
      Size = Opcode == 1 ? aarch64::WordBytes : aarch64::InstructionBytes;
      if (Vector)
        Size = aarch64::InstructionBytes << Opcode;
      Address = I.address + uint64_t(llvm::SignExtend64<LiteralOffsetBits>(
                                Word >> LiteralOffsetShift)) *
                                aarch64::InstructionBytes;
      Accesses.push_back({Address, {}, Size, Read});
    } else
      return llvm::make_error<UnsupportedExecutionError>();
  }
  for (const auto &M : Accesses)
    if (M.Size - 1 > UINT64_MAX - M.Address)
      return llvm::make_error<UnsupportedExecutionError>();
  return Accesses;
}
} // namespace neverd::emulation
