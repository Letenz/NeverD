#ifndef NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H
#define NEVERD_IR_MED_MEDCONSTANTPROPAGATION_H

namespace neverd {
struct MedFunc;

/// Propagate equal, fully seeded constants through pure same-width SSA copies
/// and complete PHIs. Preserve occurrence-sensitive address provenance and
/// reject uninitialized cycles. Returns whether any operand changed; exhausted
/// analysis budgets leave the function unchanged.
bool propagateInvariantConstants(MedFunc &Func);
} // namespace neverd

#endif
