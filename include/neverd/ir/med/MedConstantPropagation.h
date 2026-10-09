#ifndef NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H
#define NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H

#include <set>

namespace neverd {
struct MedFunc;
struct MedBlock;
struct PhiNode;
struct BinaryImage;

/// A PHI supplies one same-width value for every actual ordinary incoming
/// edge, with no extra, duplicate, or implicit exceptional entry value.
bool hasCompleteOrdinaryPhiInputs(const MedBlock &Block, const PhiNode &Phi,
                                  const std::set<int> &Incoming);

/// Propagate equal, fully seeded constants through pure same-width SSA copies
/// and complete PHIs. Preserve occurrence-sensitive address provenance and
/// reject uninitialized cycles. Returns whether any operand changed; exhausted
/// analysis budgets leave the function unchanged.
bool propagateInvariantConstants(MedFunc &Func);

/// Evaluate bounded, side-effect-free scans of immutable linked-image tables.
/// A scan is replaced only after every iteration and its exit values have
/// been proved. Mutable/volatile memory, unknown control, and exhausted bounds
/// leave that loop intact.
bool foldImmutableTableScans(MedFunc &Func, const BinaryImage &Image);
} // namespace neverd

#endif
