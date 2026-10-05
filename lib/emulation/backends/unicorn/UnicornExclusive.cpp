//===- UnicornExclusive.cpp - Shared ARM64 exclusive instruction bridge --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "UnicornExclusive.h"

#include "../../core/ExecutionDiagnostics.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
llvm::Error
executeUnicornExclusive(uc_engine *Engine, uint64_t PC,
                        MemoryProjection &Memory,
                        std::shared_ptr<RAMReservation> &Exclusive,
                        const AArch64ExclusiveAccess &Access,
                        llvm::function_ref<int(CPURegister)> RegisterID) {
  std::array<uint8_t, aarch64::InstructionBytes> Bytes;
  if (auto E = Memory.read(PC, Bytes, Execute))
    return E;
  const uint32_t Word = llvm::support::endian::read32le(Bytes.data());
  if (!isAArch64Exclusive(Word))
    return llvm::Error::success();
  AArch64MachineState Before;
  for (unsigned R = 0; R <= aarch64::GPRCount; ++R) {
    const auto Register = CPURegister(unsigned(CPURegister::AArch64X0) + R);
    if (uc_reg_read(Engine, RegisterID(Register), &Before.Registers[R]) !=
        UC_ERR_OK)
      return diagnostic::error(diagnostic::UnicornReadRegister);
  }
  Before.reg(AArch64Register::PC) = PC;
  auto Decoded = decodeAArch64Exclusive(Word, Before);
  if (!Decoded)
    return Decoded.takeError();
  const auto &I = **Decoded;
  if (I.Kind != AArch64ExclusiveInstruction::Operation::Clear) {
    auto Page = Memory.mappings().find(I.Address & ~(memory::PageSize - 1));
    if (Page != Memory.mappings().end() && Page->second.IO)
      return llvm::make_error<UnsupportedExecutionError>();
  }
  auto Next = Before;
  if (auto E = executeAArch64Exclusive(I, Next, Exclusive, Memory, Access))
    return E;
  if (Next.reg(AArch64Register::PC) == PC)
    return llvm::Error::success();
  if (I.Kind == AArch64ExclusiveInstruction::Operation::Store) {
    auto Target = Memory.mappings().find(I.Address & ~(memory::PageSize - 1));
    // Completion writes the shared backing directly. Every executable alias
    // must retire its translated blocks before a same-run branch reaches it.
    if (Target != Memory.mappings().end())
      for (const auto &[Address, Page] : Memory.mappings())
        if (Page.Physical == Target->second.Physical &&
            (Page.Permissions & Execute))
          if (auto Status = uc_ctl_remove_cache(Engine, Address,
                                                Address + memory::PageSize - 1);
              Status != UC_ERR_OK)
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           uc_strerror(Status));
  }
  // Publish an already committed instruction even if cancellation raced with
  // the commit. Writing PC makes Unicorn discard this original instruction.
  for (unsigned R = 0; R <= aarch64::GPRCount; ++R) {
    const auto Register = CPURegister(unsigned(CPURegister::AArch64X0) + R);
    if (Next.Registers[R] != Before.Registers[R] &&
        uc_reg_write(Engine, RegisterID(Register), &Next.Registers[R]) !=
            UC_ERR_OK)
      return diagnostic::error(diagnostic::UnicornWriteRegister);
  }
  if (uc_reg_write(Engine, RegisterID(CPURegister::AArch64PC),
                   &Next.reg(AArch64Register::PC)) != UC_ERR_OK)
    return diagnostic::error(diagnostic::UnicornWriteRegister);
  return llvm::Error::success();
}

void observeUnicornRAMWrite(MemoryProjection &Memory, uint64_t Address,
                            uint64_t Size) {
  while (Size) {
    const uint64_t Offset = Address % memory::PageSize;
    const uint64_t Count = std::min(Size, memory::PageSize - Offset);
    auto Page = Memory.mappings().find(Address - Offset);
    if (Page != Memory.mappings().end() && !Page->second.IO)
      Memory.recordRAMWrite(Page->second.Physical + Offset, Count);
    Size -= Count;
    if (Count > UINT64_MAX - Address)
      break;
    Address += Count;
  }
}
} // namespace neverd::emulation
