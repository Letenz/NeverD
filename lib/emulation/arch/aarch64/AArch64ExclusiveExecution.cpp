//===- AArch64ExclusiveExecution.cpp - Shared exclusive completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/RAMTransaction.h"
#include "AArch64Exclusive.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace neverd::emulation {
llvm::Error executeAArch64Exclusive(const AArch64ExclusiveInstruction &I,
                                    AArch64MachineState &CPU,
                                    std::shared_ptr<RAMReservation> &Exclusive,
                                    MemoryProjection &Memory,
                                    const AArch64ExclusiveAccess &Access) {
  const auto &Hooks = Access.Hooks;
  using Operation = AArch64ExclusiveInstruction::Operation;
  auto Next = CPU;
  Next.reg(AArch64Register::PC) += aarch64::InstructionBytes;
  auto WriteRegister = [&](unsigned Register, uint64_t Value) {
    if (Register != aarch64::GPRCount)
      Next.Registers[Register] = Value;
  };
  if (I.Kind == Operation::Clear) {
    if (!Access.Stopped()) {
      Exclusive.reset();
      CPU = Next;
    }
    return llvm::Error::success();
  }
  const unsigned Size = I.size();
  const bool Load = I.Kind == Operation::Load;
  // Checked execution selects FEAT_LSE2's single-copy atomic quantity. Raw
  // Unicorn retains its engine's baseline alignment/feature-register model.
  // Alignment faults precede permissions even when the monitor has expired.
  if (!isAArch64ExclusiveAligned(I.Address, Size, Access.Alignment)) {
    BackendFault Fault{
        BackendFaultKind::Alignment, CPU.reg(AArch64Register::PC), I.Address,
        Size, Load ? BackendAccessKind::Read : BackendAccessKind::Write};
    Fault.Cause = BackendFaultCause::OperandAlignment;
    return Access.RaiseFault(Fault);
  }
  if (Load && Hooks.Read)
    Hooks.Read(I.Address, Size);
  if (Access.Stopped())
    return llvm::Error::success();
  // The architecture permits permission checks before a failed local monitor.
  // This matches the native Windows ARM64 observations, including read-only
  // and inaccessible destinations after CLREX.
  if (auto E = Access.CheckAccess(I.Address, Size, Load ? Read : Write))
    return E;
  if (Access.Stopped())
    return llvm::Error::success();
  std::array<uint8_t, aarch64::ExclusiveGranule> Bytes{};
  if (Load) {
    if (auto E = Memory.read(I.Address,
                             llvm::MutableArrayRef(Bytes).take_front(Size)))
      return E;
    auto Reservation =
        Memory.reserveRAM(I.Address, Size, aarch64::ExclusiveGranule);
    if (!Reservation)
      return Reservation.takeError();
    for (unsigned N = 0; N < I.Count; ++N) {
      uint64_t Value = 0;
      for (unsigned B = 0; B < I.Width; ++B)
        Value |= uint64_t(Bytes[N * I.Width + B]) << (B * CHAR_BIT);
      WriteRegister(N ? I.Second : I.First, Value);
    }
    if (!Access.Stopped()) {
      Exclusive = std::move(*Reservation);
      CPU = Next;
    }
    return llvm::Error::success();
  }
  auto Matches = Memory.reservationMatches(Exclusive, I.Address, Size);
  if (!Matches)
    return Matches.takeError();
  WriteRegister(I.Status, *Matches ? aarch64::ExclusiveSuccess
                                   : aarch64::ExclusiveFailure);
  if (*Matches) {
    for (unsigned N = 0; N < I.Count; ++N) {
      const unsigned Register = N ? I.Second : I.First;
      const uint64_t Value =
          Register == aarch64::GPRCount ? 0 : CPU.Registers[Register];
      for (unsigned B = 0; B < I.Width; ++B)
        Bytes[N * I.Width + B] = uint8_t(Value >> (B * CHAR_BIT));
    }
    if (Hooks.Write)
      for (unsigned Offset = 0; Offset < Size; Offset += aarch64::WordBytes) {
        const unsigned Count =
            std::min<unsigned>(Size - Offset, aarch64::WordBytes);
        uint64_t Value = 0;
        for (unsigned B = 0; B < Count; ++B)
          Value |= uint64_t(Bytes[Offset + B]) << (B * CHAR_BIT);
        Hooks.Write(I.Address + Offset, Count, Value);
        if (Access.Stopped())
          return llvm::Error::success();
      }
    const RAMWriteRange Range{I.Address, Size};
    auto Transaction =
        RAMTransaction::create(Memory, Range, Size, Access.WritePermissions);
    if (!Transaction)
      return Transaction.takeError();
    const auto &Page =
        Memory.mappings().at(I.Address & ~(memory::PageSize - 1));
    std::memcpy(
        Memory.physicalPointer(Page.Physical + I.Address % memory::PageSize),
        Bytes.data(), Size);
    if (auto E = (*Transaction)->stage())
      return E;
    if (Access.Stopped())
      return llvm::Error::success();
    if (auto E = (*Transaction)->commit())
      return E;
  }
  if (!*Matches && Access.Stopped())
    return llvm::Error::success();
  Exclusive.reset();
  CPU = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
