//===- AArch64AtomicExecution.cpp - Commit LSE effects under a RAM lease
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/RAMTransaction.h"
#include "AArch64Atomic.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace neverd::emulation {
llvm::Error executeAArch64Atomic(const AArch64AtomicInstruction &I,
                                 AArch64MachineState &CPU,
                                 MemoryProjection &Memory,
                                 const AArch64AtomicAccess &Access) {
  using Operation = AArch64AtomicInstruction::Operation;
  const unsigned Size = I.size();
  if (!isAArch64AtomicAligned(I.Address, Size, Access.Alignment)) {
    BackendFault Fault{BackendFaultKind::Alignment,
                       CPU.reg(AArch64Register::PC), I.Address, Size,
                       BackendAccessKind::Write};
    Fault.Cause = BackendFaultCause::OperandAlignment;
    return Access.RaiseFault(Fault);
  }
  const auto Page = Memory.mappings().find(I.Address & ~(memory::PageSize - 1));
  if (Page != Memory.mappings().end() && Page->second.IO)
    return llvm::make_error<UnsupportedExecutionError>();
  if (Access.Hooks.Read)
    Access.Hooks.Read(I.Address, Size);
  if (Access.Stopped())
    return llvm::Error::success();
  // Every CAS checks write permission, including a comparison mismatch.
  if (auto E = Access.CheckAccess(I.Address, Size, Read | Write))
    return E;
  if (Access.Stopped())
    return llvm::Error::success();
  std::array<uint8_t, aarch64::AtomicAlignmentGranule> Bytes{};
  if (auto E =
          Memory.read(I.Address, llvm::MutableArrayRef(Bytes).take_front(Size)))
    return E;
  const uint64_t Mask = llvm::maskTrailingOnes<uint64_t>(I.Width * CHAR_BIT);
  auto Register = [&](unsigned R) {
    return R == aarch64::GPRCount ? uint64_t(0) : CPU.Registers[R] & Mask;
  };
  auto MemoryValue = [&](unsigned N) {
    uint64_t Value = 0;
    for (unsigned B = 0; B < I.Width; ++B)
      Value |= uint64_t(Bytes[N * I.Width + B]) << (B * CHAR_BIT);
    return Value;
  };
  bool Matches = true;
  if (I.compare())
    for (unsigned N = 0; N < I.Count; ++N)
      Matches &= MemoryValue(N) == Register(I.Source + N);
  auto Next = CPU;
  Next.reg(AArch64Register::PC) += aarch64::InstructionBytes;
  for (unsigned N = 0; N < I.Count; ++N) {
    const uint64_t Old = MemoryValue(N), Source = Register(I.Source + N);
    const uint64_t Sign = uint64_t(1) << (I.Width * CHAR_BIT - 1);
    // Biasing the sign bit compares signed operands without narrowing a host
    // signed type. Returned old values are always zero-extended.
    const bool SignedLess = (Old ^ Sign) < (Source ^ Sign);
    uint64_t Value = 0;
    switch (I.Kind) {
    case Operation::Add:
      Value = Old + Source;
      break;
    case Operation::Clear:
      Value = Old & ~Source;
      break;
    case Operation::Xor:
      Value = Old ^ Source;
      break;
    case Operation::Set:
      Value = Old | Source;
      break;
    case Operation::SignedMax:
      Value = SignedLess ? Source : Old;
      break;
    case Operation::SignedMin:
      Value = SignedLess ? Old : Source;
      break;
    case Operation::UnsignedMax:
      Value = std::max(Old, Source);
      break;
    case Operation::UnsignedMin:
      Value = std::min(Old, Source);
      break;
    case Operation::Swap:
      Value = Source;
      break;
    case Operation::Compare:
    case Operation::ComparePair:
      // Arm permits an old-value writeback on a failed comparison. This
      // profile selects it explicitly, also invalidating physical monitors.
      Value = Matches ? Register(I.Target + N) : Old;
      break;
    }
    for (unsigned B = 0; B < I.Width; ++B)
      Bytes[N * I.Width + B] = uint8_t(Value >> (B * CHAR_BIT));
    const unsigned Result = (I.compare() ? I.Source : I.Target) + N;
    if (Result != aarch64::GPRCount)
      Next.Registers[Result] = Old;
  }
  if (Access.Hooks.Write)
    for (unsigned Offset = 0; Offset < Size; Offset += aarch64::WordBytes) {
      const unsigned Count =
          std::min<unsigned>(Size - Offset, aarch64::WordBytes);
      uint64_t Value = 0;
      for (unsigned B = 0; B < Count; ++B)
        Value |= uint64_t(Bytes[Offset + B]) << (B * CHAR_BIT);
      Access.Hooks.Write(I.Address + Offset, Count, Value);
      if (Access.Stopped())
        return llvm::Error::success();
    }
  const RAMWriteRange Range{I.Address, Size};
  auto Transaction =
      RAMTransaction::create(Memory, Range, Size, Access.WritePermissions);
  if (!Transaction)
    return Transaction.takeError();
  std::memcpy(Memory.physicalPointer(Page->second.Physical +
                                     I.Address % memory::PageSize),
              Bytes.data(), Size);
  if (auto E = (*Transaction)->stage())
    return E;
  if (Access.Stopped())
    return llvm::Error::success();
  if (auto E = (*Transaction)->commit())
    return E;
  CPU = Next;
  return llvm::Error::success();
}
} // namespace neverd::emulation
