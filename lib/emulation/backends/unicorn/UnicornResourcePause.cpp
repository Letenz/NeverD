//===- UnicornResourcePause.cpp - Admit proven restart shapes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Admit proven restart shapes.
///

#include "UnicornResourcePause.h"

#include "../../core/MemoryProjection.h"

#include "llvm/ADT/ScopeExit.h"

#include <array>
#include <capstone/capstone.h>

namespace neverd::emulation {
namespace {
bool integerRegister(unsigned R) {
  switch (R) {
#define NEVERD_RESOURCE_GPR(Name)                                              \
  case X86_REG_##Name:                                                         \
    return true;
#include "UnicornResourceRegisters.def"
#undef NEVERD_RESOURCE_GPR
  default:
    return false;
  }
}
} // namespace
std::optional<ResourcePause> classifyUnicornResourcePause(
    MemoryProjection &Memory, GuestArchitecture Architecture,
    uint64_t InstructionPC, const BackendFault &Fault, bool InstructionWrote,
    bool InstructionTouchedMMIO) {
  if (Architecture != GuestArchitecture::X64 ||
      Fault.Kind != BackendFaultKind::UnmappedMemory || !Fault.Address ||
      !Fault.Size || !*Fault.Size || !Fault.Access || InstructionTouchedMMIO)
    return std::nullopt;
  if (*Fault.Access == BackendAccessKind::Execute)
    return ResourcePause{Fault,         Fault.PC,
                         InstructionPC, ResourceRestartKind::FetchBoundary,
                         true,          Execute};
  if (Fault.PC != InstructionPC)
    return std::nullopt;
  std::array<uint8_t, 15> Bytes{};
  size_t Size = Bytes.size();
  if (Size - 1 > UINT64_MAX - InstructionPC)
    Size = UINT64_MAX - InstructionPC + 1;
  if (auto Missing = Memory.firstAccessFailure(InstructionPC, Size, Execute))
    Size = Missing->Address - InstructionPC;
  if (!Size)
    return std::nullopt;
  if (auto E =
          Memory.read(InstructionPC,
                      llvm::MutableArrayRef(Bytes).take_front(Size), Execute)) {
    llvm::consumeError(std::move(E));
    return std::nullopt;
  }
  csh Decoder = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &Decoder) != CS_ERR_OK)
    return std::nullopt;
  auto Close = llvm::scope_exit([&] { cs_close(&Decoder); });
  if (cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return std::nullopt;
  cs_insn *Instruction = nullptr;
  if (cs_disasm(Decoder, Bytes.data(), Size, InstructionPC, 1, &Instruction) !=
      1)
    return std::nullopt;
  auto Free = llvm::scope_exit([&] { cs_free(Instruction, 1); });
  const auto &I = *Instruction;
  const auto &X = I.detail->x86;
  // Byte iterations have no partially written element. RAM reads can be
  // repeated without a device effect; RCX/RSI/RDI retain completed progress.
  if (I.id == X86_INS_MOVSB && I.size == 2 && I.bytes[0] == 0xF3 &&
      I.bytes[1] == 0xA4 && *Fault.Size == 1)
    return ResourcePause{
        Fault,         Fault.PC,
        InstructionPC, ResourceRestartKind::RepeatProgress,
        true,          *Fault.Access == BackendAccessKind::Read ? Read : Write};
  // These scalar loads have one memory input and one integer destination;
  // no memory or implicit stack/flag effect precedes a failed read. Other
  // forms require their own admission evidence and remain terminal.
  if ((I.id == X86_INS_MOV || I.id == X86_INS_MOVZX || I.id == X86_INS_MOVSX ||
       I.id == X86_INS_MOVSXD) &&
      *Fault.Access == BackendAccessKind::Read && !InstructionWrote &&
      X.op_count == 2 && X.operands[0].type == X86_OP_REG &&
      integerRegister(X.operands[0].reg) && X.operands[0].size <= 8 &&
      X.operands[1].type == X86_OP_MEM && X.operands[1].size <= 8 &&
      X.prefix[0] == 0)
    return ResourcePause{Fault,         Fault.PC,
                         InstructionPC, ResourceRestartKind::ArchitecturalRetry,
                         false,         Read};
  return std::nullopt;
}
} // namespace neverd::emulation
