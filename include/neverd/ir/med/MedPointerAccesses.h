//===- MedPointerAccesses.h - What code does through a pointer --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What a function's code does through each pointer value it holds: the
/// loads and stores whose address is the value plus a constant, and the
/// addresses the value plus a constant takes that the code passes on, as
/// `&p->List` does.  These are the evidence a search for the type of the
/// pointer weighs (types::fitTypes).  They describe the code; they prove
/// no type.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDPOINTERACCESSES_H
#define NEVERD_IR_MED_MEDPOINTERACCESSES_H

#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace neverd {

/// One thing the code does through a pointer.
struct MedPointerAccess {
  enum class Use : uint8_t { Read, Write, Address };
  /// The instruction that does it.
  va_t Addr = 0;
  /// Bytes from where the pointer points, and how many it reads or writes
  /// there: none when the code takes the address.
  int64_t Offset = 0;
  uint16_t Bytes = 0;
  Use Kind = Use::Read;
  bool operator==(const MedPointerAccess &) const = default;
};

/// The accesses of one function through each pointer value, by the SSA
/// value an address is a constant away from.
class MedPointerAccesses {
public:
  explicit MedPointerAccesses(const MedFunc &Func);

  /// The pointer the memory operand of the instruction at \p Addr goes
  /// through, and what it does there: its load or store, else the address
  /// it computes and passes on, as `lea` does.  None for an instruction
  /// that reaches memory by a constant address or not at all.
  std::optional<std::pair<MedVar, MedPointerAccess>> at(va_t Addr) const;

  /// Every access through \p Base, in the order of the code's addresses.
  std::vector<MedPointerAccess> through(const MedVar &Base) const;

  /// Every pointer value some access goes through.
  std::vector<MedVar> bases() const;

  /// Where \p Address points: the SSA value it is a constant away from,
  /// through copies and additions or subtractions of constants, and that
  /// constant.  None for a constant address.
  std::optional<std::pair<MedVar, int64_t>> baseOf(const MedVar &Address) const;

private:
  using Key = std::tuple<int, int, int>;
  static Key keyOf(const MedVar &Var) {
    return {static_cast<int>(Var.Kind), Var.Id, Var.SSAVer};
  }
  std::map<Key, const MedOp *> Definitions;
  std::map<Key, std::vector<MedPointerAccess>> ByBase;
  std::map<Key, MedVar> Bases;
  std::vector<std::pair<MedVar, MedPointerAccess>> ByInstruction;
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDPOINTERACCESSES_H
