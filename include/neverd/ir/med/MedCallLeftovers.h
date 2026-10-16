//===- MedCallLeftovers.h - Values calls leave in registers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The values a function's registers hold only because earlier calls left
/// them there.  The ABI lets a call change a caller-saved register, so what
/// the register holds afterwards is no value the function sets, and no
/// compiler passes it on as an argument.  Call-argument recovery and HighIR's
/// call arguments both end a call's arguments at such a register.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLLEFTOVERS_H
#define NEVERD_IR_MED_MEDCALLLEFTOVERS_H

#include "neverd/ir/med/MedIR.h"

#include <set>
#include <tuple>

namespace neverd {

/// Each call's clobber of a caller-saved register it may change whole
/// (MedFunc::CallClobbers), and each join of nothing else.  A clobber the
/// struct-return recovery still holds as a possible result field
/// (MedFunc::StructReturnCandidates) is none: its call may return part of
/// its result there.
class ValuesLeftByCalls {
public:
  explicit ValuesLeftByCalls(const MedFunc &Func);

  /// Whether \p V holds only what earlier calls left in its register.
  bool contains(const MedVar &V) const;

private:
  std::set<std::tuple<int, int, int>> Values;
};

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLLEFTOVERS_H
