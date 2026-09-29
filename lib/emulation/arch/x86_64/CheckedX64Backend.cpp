//===- CheckedX64Backend.cpp - Checked x64 execution---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedX64Backend.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/ADT/ScopeExit.h"

#include <chrono>
#include <exception>
#include <utility>

namespace neverd::emulation {
using diagnostic::error;
bool CheckedX64Backend::canonicalRange(uint64_t A, uint64_t N) const {
  return x64::canonicalRange(A, N);
}

llvm::Expected<std::unique_ptr<ExecutionBackend>>
CheckedX64Backend::create(std::unique_ptr<MemoryProjection> Memory,
                          std::unique_ptr<X64Machine> Machine) {
  auto B = std::unique_ptr<CheckedX64Backend>(new CheckedX64Backend());
  B->Memory = std::move(Memory);
  B->Machine = std::move(Machine);
  if (auto E = B->Memory->validateMappings(x64::canonicalRange))
    return E;
  if (auto E = B->initializeDecoder(CS_ARCH_X86, CS_MODE_64))
    return E;
  B->CPU.reg(X64Register::FLAGS) = x64::InitialFlags;
  B->CPU.reg(X64Register::CS) = x64::CodeSelector;
  B->CPU.reg(X64Register::SS) = x64::DataSelector;
  return std::unique_ptr<ExecutionBackend>(std::move(B));
}

llvm::Expected<RegisterValue> CheckedX64Backend::readRegister(CPURegister R) {
  if (!registerMatches(R, architecture()))
    return error(diagnostic::Register);
  if (R == CPURegister::X64GSBase)
    return RegisterValue{CPU.GSBase, 0};
  if (R >= CPURegister::X64V0 && R <= CPURegister::X64V15)
    return CPU.Xmm[unsigned(R) - unsigned(CPURegister::X64V0)];
  return RegisterValue{CPU.Registers[unsigned(R)], 0};
}
llvm::Error CheckedX64Backend::writeRegister(CPURegister R,
                                             const RegisterValue &V) {
  if (auto E = mutableMemory())
    return E;
  if (!registerMatches(R, architecture()))
    return error(diagnostic::Register);
  if (R >= CPURegister::X64V0 && R <= CPURegister::X64V15) {
    CPU.Xmm[unsigned(R) - unsigned(CPURegister::X64V0)] = V;
    return llvm::Error::success();
  }
  if (V[1] || (R == CPURegister::X64CR8 && V[0] > x64::MaxCR8) ||
      (R == CPURegister::X64CS && V[0] != x64::CodeSelector) ||
      (R == CPURegister::X64SS && V[0] != x64::DataSelector) ||
      (R == CPURegister::X64FLAGS &&
       ((V[0] & ~x64::AllowedFlags) || !(V[0] & x64::ReservedFlag))) ||
      (R == CPURegister::X64GSBase && !x64::canonical(V[0])))
    return error(diagnostic::Register);
  if (R == CPURegister::X64GSBase)
    CPU.GSBase = V[0];
  else
    CPU.Registers[unsigned(R)] = V[0];
  return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<BackendContext>>
CheckedX64Backend::saveContext() {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  auto S = std::make_unique<SavedState>();
  S->Owner = Identity;
  S->Space = addressSpace();
  S->CPU = CPU;
  return makeContext(std::move(S));
}

llvm::Error CheckedX64Backend::saveContext(BackendContext &C) {
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

llvm::Error CheckedX64Backend::restoreContext(const BackendContext &C) {
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

llvm::Expected<uint64_t> CheckedX64Backend::operandRegister(unsigned R) const {
  if (R == X86_REG_INVALID)
    return uint64_t(0);
  switch (R) {
#define NEVERD_X64_OPERAND_REGISTER(ID, Name, Shift, Bits)                     \
  case X86_REG_##ID:                                                           \
    return (CPU.reg(X64Register::Name) >> Shift) &                             \
           (UINT64_MAX >> (x64::WordBits - Bits));
#include "X64OperandRegisters.def"
#undef NEVERD_X64_OPERAND_REGISTER
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
}

llvm::Error CheckedX64Backend::execute(const cs_insn &I) {
  switch (I.id) {
#define NEVERD_CHECKED_X64_INSTRUCTION(Name)                                   \
  case X86_INS_##Name:                                                         \
    break;
#include "CheckedX64Instructions.def"
#undef NEVERD_CHECKED_X64_INSTRUCTION
  default:
    return llvm::make_error<UnsupportedExecutionError>();
  }
  const auto &X = I.detail->x86;
  if (X.prefix[0] ||
      (X.addr_size != x64::DWordBytes && X.addr_size != x64::WordBytes) ||
      ((I.id == X86_INS_RET || I.id == X86_INS_CALL) &&
       X.prefix[2] == X86_PREFIX_OPSIZE))
    return llvm::make_error<UnsupportedExecutionError>();
  // The 16-bit BSWAP encoding has undefined architectural results.
  if (I.id == X86_INS_BSWAP &&
      (X.op_count != 1 || (X.operands[0].size != x64::DWordBytes &&
                           X.operands[0].size != x64::WordBytes)))
    return llvm::make_error<UnsupportedExecutionError>();
  struct Access {
    uint64_t Address;
    unsigned Size, Permission;
    uint64_t Value;
  };
  std::vector<Access> Accesses;
  auto Value = [&](const cs_x86_op &O) -> llvm::Expected<uint64_t> {
    if (O.type == X86_OP_IMM)
      return uint64_t(O.imm);
    if (O.type == X86_OP_REG)
      return operandRegister(O.reg);
    return llvm::make_error<UnsupportedExecutionError>();
  };
  for (unsigned N = 0; N < X.op_count; ++N) {
    const auto &O = X.operands[N];
    if (O.type == X86_OP_REG) {
      auto V = operandRegister(O.reg);
      if (!V)
        return V.takeError();
    }
    if (O.type != X86_OP_MEM || I.id == X86_INS_LEA || I.id == X86_INS_NOP)
      continue;
    if (O.size > x64::WordBytes || !O.size ||
        (O.mem.segment != X86_REG_INVALID && O.mem.segment != X86_REG_DS &&
         O.mem.segment != X86_REG_SS && O.mem.segment != X86_REG_ES &&
         O.mem.segment != X86_REG_GS))
      return llvm::make_error<UnsupportedExecutionError>();
    auto B = operandRegister(O.mem.base), Index = operandRegister(O.mem.index);
    if (!B) {
      if (!Index)
        llvm::consumeError(Index.takeError());
      return B.takeError();
    }
    if (!Index)
      return Index.takeError();
    uint64_t A = *B + *Index * O.mem.scale + O.mem.disp;
    if (O.mem.base == X86_REG_RIP || O.mem.base == X86_REG_EIP)
      A += I.size;
    if (X.addr_size == x64::DWordBytes)
      A = uint32_t(A);
    if (O.mem.segment == X86_REG_GS)
      A += CPU.GSBase;
    if (O.access == CS_AC_READ)
      Accesses.push_back({A, O.size, Read, 0});
    else if (O.access == CS_AC_WRITE &&
             (I.id == X86_INS_MOV || I.id == X86_INS_MOVABS) && N == 0 &&
             X.op_count == 2) {
      auto V = Value(X.operands[1]);
      if (!V)
        return V.takeError();
      Accesses.push_back({A, O.size, Write, *V});
    } else
      return llvm::make_error<UnsupportedExecutionError>();
  }
  uint64_t SP = CPU.reg(X64Register::SP);
  if (I.id == X86_INS_PUSH || I.id == X86_INS_CALL) {
    uint64_t V = I.address + I.size;
    if (I.id == X86_INS_PUSH) {
      if (X.op_count != 1 || X.operands[0].size != x64::WordBytes)
        return llvm::make_error<UnsupportedExecutionError>();
      auto Source = Value(X.operands[0]);
      if (!Source)
        return Source.takeError();
      V = *Source;
    }
    Accesses.push_back({SP - x64::WordBytes, x64::WordBytes, Write, V});
  }
  if (I.id == X86_INS_POP || I.id == X86_INS_RET) {
    if (I.id == X86_INS_POP &&
        (X.op_count != 1 || X.operands[0].type != X86_OP_REG ||
         X.operands[0].size != x64::WordBytes))
      return llvm::make_error<UnsupportedExecutionError>();
    Accesses.push_back({SP, x64::WordBytes, Read, 0});
  }
  // Single-page transactions are the initial contract. Cross-page partial
  // effects and MMIO are not approximated by a sequence of host callbacks.
  for (const auto &A : Accesses)
    if (A.Size - 1 > UINT64_MAX - A.Address ||
        A.Address / x64::PageSize != (A.Address + A.Size - 1) / x64::PageSize)
      return llvm::make_error<UnsupportedExecutionError>();
  for (const auto &A : Accesses) {
    if (A.Permission == Read && Hooks.Read)
      Hooks.Read(A.Address, A.Size);
    if (A.Permission == Write && Hooks.Write)
      Hooks.Write(A.Address, A.Size, A.Value);
    if (StopRequested || FirstFault)
      return llvm::Error::success();
    if (auto E = access(A.Address, A.Size, A.Permission, true))
      return E;
    if (StopRequested)
      return llvm::Error::success();
  }
  auto Root = buildX64PageTables(*Memory, PageTableRoot);
  if (!Root)
    return Root.takeError();
  PageTableRoot = *Root;
  return Machine->step(CPU, PageTableRoot, Deadline);
}

} // namespace neverd::emulation
