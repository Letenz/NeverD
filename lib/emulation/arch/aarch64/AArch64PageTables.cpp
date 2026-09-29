//===- AArch64PageTables.cpp - ARM64 address-space projection -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/ExecutionDiagnostics.h"
#include "AArch64Machine.h"

#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::emulation {
llvm::Error buildAArch64PageTables(MemoryProjection &Memory) {
  if (!Memory.needsProjection())
    return llvm::Error::success();
  if (auto E = Memory.validateMappings(aarch64::canonicalRange))
    return E;
  using namespace aarch64;
  std::memset(Memory.data(), 0, memory::ProjectionReserve);
  // This immutable, backend-owned exception gateway never overlaps guest RAM.
  for (uint64_t Offset = 0; Offset < VectorTableSize; Offset += VectorStride)
    llvm::support::endian::write32le(Memory.data() + VectorGPA + Offset,
                                     GatewayHypercall);
  llvm::support::endian::write32le(Memory.data() + ProbePC, ProbeInstruction);
  uint64_t Offset = 0;
  for (uint32_t Instruction : Maintenance) {
    llvm::support::endian::write32le(Memory.data() + EntryGPA + Offset,
                                     Instruction);
    Offset += InstructionBytes;
  }
#define NEVERD_AARCH64_GATE_RETURN(Name, Encoding)                             \
  llvm::support::endian::write32le(Memory.data() + EntryGPA + Offset, Encoding);
#include "AArch64Machine.def"
#undef NEVERD_AARCH64_GATE_RETURN
  uint64_t Next = FirstChildTable;
  auto Map = [&](uint64_t VA, uint64_t PA,
                 unsigned Permissions) -> llvm::Error {
    uint64_t Table = VA <= UserMax ? LowRoot : HighRoot;
    for (unsigned Level = TableLevels; Level > 1; --Level) {
      unsigned Index =
          (VA >> (PageBits + (Level - 1) * TableBits)) & (TableEntries - 1);
      auto *Slot = Memory.data() + Table + Index * WordBytes;
      uint64_t Entry = llvm::support::endian::read64le(Slot);
      if (!Entry) {
        if (Next == memory::ProjectionReserve)
          return diagnostic::error(diagnostic::PageTables);
        Entry = Next | TableDescriptor;
        Next += memory::PageSize;
        llvm::support::endian::write64le(Slot, Entry);
      }
      Table = Entry & AddressMask;
    }
    uint64_t Entry =
        PA | TableDescriptor | AccessFlag | InnerShareable | UserNX;
    if (!Permissions)
      Entry = 0;
    else {
      if (!(Permissions & Write))
        Entry |= ReadOnly;
      if (!(Permissions & Execute))
        Entry |= PrivilegedNX;
    }
    unsigned Index = (VA >> PageBits) & (TableEntries - 1);
    llvm::support::endian::write64le(Memory.data() + Table + Index * WordBytes,
                                     Entry);
    return llvm::Error::success();
  };
  if (auto E = Map(VectorGPA, VectorGPA, Read | Execute))
    return E;
  if (auto E = Map(EntryGPA, EntryGPA, Read | Execute))
    return E;
  for (const auto &[VA, Page] : Memory.mappings())
    if (auto E = Map(VA, Page.Physical, Page.Permissions))
      return E;
  Memory.commitProjection();
  return llvm::Error::success();
}
llvm::Error verifyAArch64Machine(AArch64Machine &Machine,
                                 MemoryProjection &Memory) {
  if (auto E = buildAArch64PageTables(Memory))
    return E;
  AArch64MachineState State;
  State.reg(AArch64Register::PC) = aarch64::ProbePC;
  auto Deadline = std::chrono::steady_clock::now() +
                  std::chrono::microseconds(aarch64::ProbeTimeoutMicroseconds);
  if (auto E = Machine.step(State, {Deadline}))
    return E;
  if (State.reg(AArch64Register::PC) !=
          aarch64::ProbePC + aarch64::InstructionBytes ||
      State.reg(AArch64Register::NZCV))
    return diagnostic::error(diagnostic::ArmState);
  return llvm::Error::success();
}
} // namespace neverd::emulation
