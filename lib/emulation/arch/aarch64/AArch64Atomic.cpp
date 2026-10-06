//===- AArch64Atomic.cpp - FEAT_LSE instruction admission -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AArch64Atomic.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_FIELD(Name, Value) constexpr unsigned Name = Value;
#define NEVERD_AARCH64_ATOMIC_FIELD NEVERD_AARCH64_FIELD
#include "AArch64AtomicInstructions.def"
#include "CheckedAArch64Instructions.def"
#undef NEVERD_AARCH64_FIELD
#undef NEVERD_AARCH64_ATOMIC_FIELD
using Operation = AArch64AtomicInstruction::Operation;
struct Encoding {
  uint32_t Mask, Value;
  Operation Kind;
  unsigned IDs[4][3];
  bool pair() const { return Kind == Operation::ComparePair; }
  unsigned sizeIndex(uint32_t Word) const { return Word >> SizeShift; }
  unsigned id(uint32_t Word) const {
    const bool Compare = Kind == Operation::Compare || pair();
    const unsigned Acquire =
        (Word >> (Compare ? CompareAcquireShift : RMWAcquireShift)) & 1;
    const unsigned Release =
        (Word >> (Compare ? CompareReleaseShift : RMWReleaseShift)) & 1;
    const unsigned Width = pair() ? 0 : std::min(sizeIndex(Word), 2u);
    return IDs[Acquire + 2 * Release][Width];
  }
};
#define ATOMIC_IDS(Name)                                                       \
  {AARCH64_INS_##Name##B, AARCH64_INS_##Name##H, AARCH64_INS_##Name}
constexpr Encoding Encodings[] = {
#define NEVERD_AARCH64_ATOMIC_RMW(Name, Kind, Mask, Value)                     \
  {Mask,                                                                       \
   Value,                                                                      \
   Operation::Kind,                                                            \
   {ATOMIC_IDS(Name), ATOMIC_IDS(Name##A), ATOMIC_IDS(Name##L),                \
    ATOMIC_IDS(Name##AL)}},
#define NEVERD_AARCH64_ATOMIC_COMPARE NEVERD_AARCH64_ATOMIC_RMW
#define NEVERD_AARCH64_ATOMIC_PAIR(Name, Kind, Mask, Value)                    \
  {Mask,                                                                       \
   Value,                                                                      \
   Operation::Kind,                                                            \
   {{AARCH64_INS_##Name},                                                      \
    {AARCH64_INS_##Name##A},                                                   \
    {AARCH64_INS_##Name##L},                                                   \
    {AARCH64_INS_##Name##AL}}},
#include "AArch64AtomicInstructions.def"
#undef NEVERD_AARCH64_ATOMIC_RMW
#undef NEVERD_AARCH64_ATOMIC_COMPARE
#undef NEVERD_AARCH64_ATOMIC_PAIR
};
#undef ATOMIC_IDS
} // namespace
bool isAArch64Atomic(uint32_t Word) {
  return llvm::any_of(
      Encodings, [Word](const auto &E) { return (Word & E.Mask) == E.Value; });
}
llvm::Expected<std::optional<AArch64AtomicInstruction>>
decodeAArch64Atomic(const cs_insn &I, const AArch64MachineState &CPU) {
  if (I.size != aarch64::InstructionBytes || !I.detail)
    return llvm::make_error<UnsupportedExecutionError>();
  const uint32_t Word = llvm::support::endian::read32le(I.bytes);
  for (const auto &E : Encodings)
    if ((Word & E.Mask) == E.Value && I.id == E.id(Word))
      return decodeAArch64Atomic(Word, CPU);
  return std::nullopt;
}
llvm::Expected<std::optional<AArch64AtomicInstruction>>
decodeAArch64Atomic(uint32_t Word, const AArch64MachineState &CPU) {
  for (const auto &E : Encodings) {
    if ((Word & E.Mask) != E.Value)
      continue;
    const unsigned Source = (Word >> IndexShift) & RegisterMask;
    const unsigned Target = Word & RegisterMask;
    const unsigned Base = (Word >> BaseShift) & RegisterMask;
    if (E.pair() &&
        (Source % PairRegisterAlignment || Target % PairRegisterAlignment))
      return llvm::make_error<UnsupportedExecutionError>();
    return AArch64AtomicInstruction{
        E.Kind,
        (E.pair() ? PairFirstWidth : 1u) << E.sizeIndex(Word),
        E.pair() ? 2u : 1u,
        Source,
        Target,
        Base == aarch64::GPRCount ? CPU.reg(AArch64Register::SP)
                                  : CPU.Registers[Base]};
  }
  return std::nullopt;
}
} // namespace neverd::emulation
