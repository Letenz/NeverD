//===- MedMutableSource.h - Bounded mutable source contract -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDMUTABLESOURCE_H
#define NEVERD_IR_MED_MEDMUTABLESOURCE_H

#include "neverd/ir/med/MedIR.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

/// A bounded, conservative contract for the explicit mutable subset retained
/// when LowToMed skips SSA. EntryBytes includes ALL upward-exposed reads; it
/// is not positive evidence that an input reaches an observable effect.
/// Variables have unique (Id, SSAVer) storage identities and fixed widths.
struct MedMutableSourcePlan {
  std::vector<MedVar> Variables;
  std::set<int> EntryValues;
  std::map<uint64_t, uint64_t> EntryBytes;
  std::optional<MedVar> ReturnValue;
};

/// Validate normalized explicit control flow, storage and standard scalar
/// returns, then compute entry demand over every CFG path. No SSA, termination,
/// source-ABI binding, native equivalence or memory-validity proof is implied.
/// Unsupported, malformed and over-budget functions return no plan.
std::optional<MedMutableSourcePlan>
analyzeMedMutableSource(const MedFunc &Function, Arch Architecture,
                        std::string *Error = nullptr);

} // namespace neverd

#endif
