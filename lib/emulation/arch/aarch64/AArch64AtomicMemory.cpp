//===- AArch64AtomicMemory.cpp - Atomic alignment and fault policy --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64AtomicMemory.h"

#include "../../core/RAMTransaction.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace neverd::emulation {
bool isAArch64AtomicAligned(uint64_t Address, uint64_t Size,
                            AArch64AtomicAlignment Alignment) {
  assert(llvm::isPowerOf2_64(Size) && Size <= aarch64::AtomicAlignmentGranule);
  if (Alignment == AArch64AtomicAlignment::Natural)
    return Address % Size == 0;
  return Address % aarch64::AtomicAlignmentGranule <=
         aarch64::AtomicAlignmentGranule - Size;
}
bool isAArch64AtomicAlignmentFault(const BackendFault &Fault) {
  return Fault.Kind == BackendFaultKind::Alignment &&
         Fault.Cause == BackendFaultCause::OperandAlignment && Fault.Address &&
         Fault.Size && llvm::isPowerOf2_64(*Fault.Size) &&
         *Fault.Size <= aarch64::AtomicAlignmentGranule &&
         *Fault.Address % *Fault.Size &&
         (Fault.Access == BackendAccessKind::Read ||
          Fault.Access == BackendAccessKind::Write) &&
         !Fault.Interrupt && !Fault.ErrorCode;
}
llvm::Expected<bool>
commitAArch64AtomicWrite(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
                         MemoryProjection &Memory,
                         const AArch64AtomicAccess &Access) {
  assert(isAArch64AtomicAligned(Address, Bytes.size(), Access.Alignment));
  if (Access.Hooks.Write)
    for (unsigned Offset = 0; Offset < Bytes.size();
         Offset += aarch64::WordBytes) {
      const unsigned Count =
          std::min<uint64_t>(Bytes.size() - Offset, aarch64::WordBytes);
      uint64_t Value = 0;
      for (unsigned B = 0; B < Count; ++B)
        Value |= uint64_t(Bytes[Offset + B]) << (B * CHAR_BIT);
      Access.Hooks.Write(Address + Offset, Count, Value);
      if (Access.Stopped())
        return false;
    }
  const RAMWriteRange Range{Address, Bytes.size()};
  auto Transaction = RAMTransaction::create(Memory, Range, Bytes.size(),
                                            Access.WritePermissions);
  if (!Transaction)
    return Transaction.takeError();
  const auto &Page = Memory.mappings().at(Address & ~(memory::PageSize - 1));
  std::memcpy(
      Memory.physicalPointer(Page.Physical + Address % memory::PageSize),
      Bytes.data(), Bytes.size());
  if (auto E = (*Transaction)->stage())
    return std::move(E);
  if (Access.Stopped())
    return false;
  if (auto E = (*Transaction)->commit())
    return std::move(E);
  return true;
}
} // namespace neverd::emulation
