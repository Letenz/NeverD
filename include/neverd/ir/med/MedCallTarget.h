//===- MedCallTarget.h - Cached callable provenance -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Observe a callable's original value through exact same-block spills. An
/// import observation identifies its original load; it never rereads an IAT.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLTARGET_H
#define NEVERD_IR_MED_MEDCALLTARGET_H

#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <map>
#include <optional>

namespace neverd {

struct MedCallTarget {
  va_t Address;
  bool ThroughSlot;
  /// Original input-block LOAD index, meaningful when ThroughSlot is true.
  std::size_t LoadIndex = 0;
};

/// Query SSA values before \p Before. Calls, uncertain aliases, partial writes
/// and ordered memory prevent a stored value from reaching a later reload.
std::optional<MedCallTarget>
resolveMedCallTarget(const MedBlock &Block, size_t Before, const MedVar &Value,
                     size_t *WorkRemaining = nullptr);

/// Give a pre-SSA block prefix private value versions before using the same
/// proof. Overlapping register writes and intervening calls kill old carriers.
/// The original operations and runtime target operand remain unchanged.
/// Observation indexes still refer to the original input block.
std::optional<MedCallTarget>
resolvePreSSAMedCallTarget(const MedBlock &Block, const MedVar &Value,
                           Arch Architecture, size_t *WorkRemaining = nullptr);

/// Adapt original machine operations to the same reaching-value proof before
/// call summaries run. Keys are original block operation indexes. Neither
/// the machine operand nor an observed import value is replaced by this query.
std::map<size_t, MedCallTarget>
resolveLowBlockCallTargets(const LowBlock &Block, Arch Architecture,
                           size_t *WorkRemaining = nullptr);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLTARGET_H
