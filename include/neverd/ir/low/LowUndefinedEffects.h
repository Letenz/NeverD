//===- LowUndefinedEffects.h - Architectural arbitrary values ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWUNDEFINEDEFFECTS_H
#define NEVERD_IR_LOW_LOWUNDEFINEDEFFECTS_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Bind a sidecar to the exact operation span, including provenance and source
/// coordinates. This detects stale reuse; it does not authenticate ISA facts.
inline std::string lowUndefinedOperationDigest(llvm::ArrayRef<LowOp> Ops) {
  llvm::SHA256 Hash;
  Hash.update("neverd-low-undefined-ops-v1");
  const auto Number = [&](uint64_t Value) {
    uint8_t Bytes[8];
    for (unsigned I = 0; I != 8; ++I)
      Bytes[I] = static_cast<uint8_t>(Value >> (I * 8));
    Hash.update(llvm::ArrayRef<uint8_t>(Bytes));
  };
  const auto Variable = [&](const NdVar &V) {
    Number(static_cast<unsigned>(V.Space));
    Number(V.Offset);
    Number(V.Size);
    Number(static_cast<unsigned>(V.Provenance));
    Number(V.AddressOwnerVA);
  };
  Number(Ops.size());
  for (const auto &Op : Ops) {
    Number(static_cast<unsigned>(Op.Opcode));
    Number(static_cast<unsigned>(Op.MemoryOrdering));
    Number(static_cast<unsigned>(Op.MemoryAddressSpace));
    Variable(Op.Output);
    Number(Op.NumInputs);
    for (const auto &Input : Op.Inputs)
      Variable(Input);
    Number(Op.Addr);
    Number(static_cast<uint64_t>(Op.Seq));
  }
  return llvm::toHex(Hash.final(), true);
}

/// Completeness of the architecture's description of newly arbitrary result
/// bits. An empty Missing record supplies no evidence. Complete covers every
/// architecturally undefined output of this exact instruction, not only flags.
enum class LowUndefinedCoverage : uint8_t { Missing, Complete, Unsupported };

/// An architectural arbitrary value is stable after production and shared by
/// all copies until overwritten. It is neither LLVM undef nor source-language
/// undefined behavior. Every dynamic execution creates a fresh production.
struct LowUndefinedEffect {
  /// Number of completed LowOps, relative to the owning instruction. Apply
  /// after those operations and before the next one; zero means before its
  /// first operation. The value must not exceed the certified OpCount.
  uint64_t AfterOp = 0;
  NdVar Output;
  uint16_t BitOffset = 0;
  uint16_t BitCount = 0;
  /// Absent means unconditional. Otherwise a constant or an instruction-local
  /// temporary already defined at AfterOp must supply an eight-bit container
  /// whose value is proved to be zero or one. Do not reread a mutable physical
  /// count register after the instruction overwrote it.
  std::optional<NdVar> When;
};

/// Owned by the architecture lifter and returned with the exact operation
/// sequence it describes. Ordinary LowIR continues to carry its selected
/// implementation values; this sidecar describes all additional ISA-allowed
/// choices for a separate relational check. Defined and preserved results do
/// not create new arbitrary producers, but retain their input dependencies.
struct LowInstructionUndefinedEffects {
  LowUndefinedCoverage Coverage = LowUndefinedCoverage::Missing;
  uint64_t OpCount = 0;
  /// Required for complete coverage; equal lengths do not imply equal code.
  std::string OperationDigest;
  std::vector<LowUndefinedEffect> Effects;
  std::string Diagnostic;
};

} // namespace neverd

#endif
