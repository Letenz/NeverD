//===- CheckedAArch64Backend.cpp - Checked ARM64 execution ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedAArch64Backend.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::emulation {
using diagnostic::error;
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
} // namespace
bool CheckedAArch64Backend::canonicalRange(uint64_t A, uint64_t N) const {
  return aarch64::canonicalRange(A, N);
}
llvm::Expected<std::unique_ptr<ExecutionBackend>>
CheckedAArch64Backend::create(std::unique_ptr<MemoryProjection> Memory,
                              std::unique_ptr<AArch64Machine> Machine) {
  auto B = std::unique_ptr<CheckedAArch64Backend>(new CheckedAArch64Backend());
  B->Memory = std::move(Memory);
  B->Machine = std::move(Machine);
  if (auto E = B->Memory->validateMappings(aarch64::canonicalRange))
    return E;
  if (auto E = B->initializeDecoder(CS_ARCH_AARCH64, CS_MODE_ARM))
    return E;
  return std::unique_ptr<ExecutionBackend>(std::move(B));
}
llvm::Expected<RegisterValue>
CheckedAArch64Backend::readRegister(CPURegister R) {
  if (!registerMatches(R, architecture()))
    return error(diagnostic::Register);
  if (R >= CPURegister::AArch64V0 && R <= CPURegister::AArch64V31)
    return CPU.Vectors[unsigned(R) - unsigned(CPURegister::AArch64V0)];
  return RegisterValue{
      CPU.Registers[unsigned(R) - unsigned(CPURegister::AArch64X0)], 0};
}
llvm::Error CheckedAArch64Backend::writeRegister(CPURegister R,
                                                 const RegisterValue &V) {
  if (auto E = mutableMemory())
    return E;
  if (!registerMatches(R, architecture()))
    return error(diagnostic::Register);
  if (R >= CPURegister::AArch64V0 && R <= CPURegister::AArch64V31) {
    CPU.Vectors[unsigned(R) - unsigned(CPURegister::AArch64V0)] = V;
    return llvm::Error::success();
  }
  if (V[1] || (registerWidth(R) == 32 && V[0] > UINT32_MAX) ||
      (R == CPURegister::AArch64NZCV && (V[0] & ~aarch64::NZCVMask)) ||
      (R == CPURegister::AArch64PC &&
       (!aarch64::canonical(V[0]) || V[0] % aarch64::InstructionBytes)))
    return error(diagnostic::Register);
  CPU.Registers[unsigned(R) - unsigned(CPURegister::AArch64X0)] = V[0];
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<BackendContext>>
CheckedAArch64Backend::saveContext() {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  auto S = std::make_unique<SavedState>();
  S->Owner = Identity;
  S->Space = addressSpace();
  S->CPU = CPU;
  return makeContext(std::move(S));
}
llvm::Error CheckedAArch64Backend::saveContext(BackendContext &C) {
  if (!contextStorage(C) || contextStorage(C)->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (contextStorage(C)->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  contextStorage(C)->Space = addressSpace();
  static_cast<SavedState &>(*contextStorage(C)).CPU = CPU;
  return llvm::Error::success();
}
llvm::Error CheckedAArch64Backend::restoreContext(const BackendContext &C) {
  if (!contextStorage(C) || contextStorage(C)->Owner.expired())
    return error(diagnostic::ContextExpired);
  if (contextStorage(C)->Owner.lock() != Identity)
    return error(diagnostic::ContextOwner);
  if (auto E = mutableMemory())
    return E;
  if (contextStorage(C)->Space.lock() != addressSpace())
    return error(diagnostic::ContextSpace);
  CPU = static_cast<const SavedState &>(*contextStorage(C)).CPU;
  TimedOut = false;
  return llvm::Error::success();
}
llvm::Error CheckedAArch64Backend::execute(const cs_insn &I) {
  using namespace encoding;
  enum InstructionKind { Integer, Memory } Kind;
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
  if (I.id == AARCH64_INS_HINT && !isNOP(Word))
    return llvm::make_error<UnsupportedExecutionError>();
  const auto &A = I.detail->aarch64;
  for (unsigned N = 0; N < A.op_count; ++N) {
    const auto &O = A.operands[N];
    if (O.type == AARCH64_OP_REG && !scalarRegister(O.reg))
      return llvm::make_error<UnsupportedExecutionError>();
    if (Kind == Integer && O.type != AARCH64_OP_REG && O.type != AARCH64_OP_IMM)
      return llvm::make_error<UnsupportedExecutionError>();
  }
  struct Access {
    uint64_t Address, Value;
    unsigned Size, Permission;
  };
  std::vector<Access> Accesses;
  auto GPR = [&](unsigned R) {
    return R == aarch64::GPRCount ? uint64_t(0) : CPU.Registers[R];
  };
  const unsigned Rt = Word & RegisterMask;
  const unsigned Rn = (Word >> BaseShift) & RegisterMask;
  const uint64_t Base =
      Rn == aarch64::GPRCount ? CPU.reg(AArch64Register::SP) : GPR(Rn);
  if (Kind == Memory) {
    bool Load = Word & LoadBit;
    unsigned Size = 1u << (Word >> SizeShift);
    uint64_t Address = Base;
    bool Writeback = false;
    if (isUnsignedMemory(Word) || isImmediateMemory(Word) ||
        isRegisterMemory(Word)) {
      unsigned Opcode = (Word >> OpcodeShift) & OpcodeMask;
      Load = Opcode != 0;
      // The size=8/opc=2 encoding is prefetch, not a scalar load.
      if (Size == aarch64::WordBytes && Opcode > 1)
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
      if (Writeback && Rn != aarch64::GPRCount && Rn == Rt)
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, GPR(Rt), Size, Load ? Read : Write});
    } else if (isPairMemory(Word)) {
      const unsigned Rt2 = (Word >> SecondShift) & RegisterMask;
      Size = (Word & PairWidthBit) ? aarch64::WordBytes
                                   : aarch64::InstructionBytes;
      const unsigned Mode = (Word >> PairModeShift) & OpcodeMask;
      Writeback = Mode == 1 || Mode == 3;
      if (Mode != 1)
        Address += uint64_t(llvm::SignExtend64<PairOffsetBits>(
                       Word >> PairOffsetShift)) *
                   Size;
      if ((Load && Rt == Rt2) ||
          (Writeback && Rn != aarch64::GPRCount && (Rn == Rt || Rn == Rt2)))
        return llvm::make_error<UnsupportedExecutionError>();
      Accesses.push_back({Address, GPR(Rt), Size, Load ? Read : Write});
      Accesses.push_back({Address + Size, GPR(Rt2), Size, Load ? Read : Write});
    } else if (isLiteralMemory(Word)) {
      const unsigned Opcode = Word >> SizeShift;
      if (Opcode == OpcodeMask)
        return llvm::make_error<UnsupportedExecutionError>();
      Size = Opcode == 1 ? aarch64::WordBytes : aarch64::InstructionBytes;
      Address = I.address + uint64_t(llvm::SignExtend64<LiteralOffsetBits>(
                                Word >> LiteralOffsetShift)) *
                                aarch64::InstructionBytes;
      Accesses.push_back({Address, 0, Size, Read});
    } else
      return llvm::make_error<UnsupportedExecutionError>();
  }
  for (const auto &M : Accesses) {
    if (M.Size - 1 > UINT64_MAX - M.Address ||
        M.Address / memory::PageSize !=
            (M.Address + M.Size - 1) / memory::PageSize)
      return llvm::make_error<UnsupportedExecutionError>();
  }
  // Validate the entire instruction before any native access or pair write.
  for (const auto &M : Accesses) {
    if (M.Permission == Read && Hooks.Read)
      Hooks.Read(M.Address, M.Size);
    if (M.Permission == Write && Hooks.Write)
      Hooks.Write(M.Address, M.Size,
                  M.Value & (UINT64_MAX >> (aarch64::WordBits - M.Size * 8)));
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    if (auto E = access(M.Address, M.Size, M.Permission, true))
      return E;
    if (StopRequested)
      return llvm::Error::success();
  }
  if (auto E = buildAArch64PageTables(*this->Memory))
    return E;
  return Machine->step(CPU, Deadline);
}
} // namespace neverd::emulation
