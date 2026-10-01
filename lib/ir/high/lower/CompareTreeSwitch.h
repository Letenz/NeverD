//===- CompareTreeSwitch.h - Switch recovery from compare trees -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Finds MedIR decision trees that compare one selector against constants,
/// the form compilers give a sparse switch (`sub`/`je` chains and binary
/// searches of `cmp`/`jg`/`je`), and describes each as one dispatch.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_HIGH_LOWER_COMPARETREESWITCH_H
#define NEVERD_LIB_IR_HIGH_LOWER_COMPARETREESWITCH_H

#include "neverd/ir/med/MedIR.h"

#include <cstdint>
#include <vector>

namespace neverd {

/// One dispatch edge of a recovered switch: the tree edge From -> To runs for
/// exactly the selector values in Values (left empty for the default edge).
/// When only this edge enters To, Region lists the blocks To dominates, in
/// index order: the code that belongs to this case alone.
struct CompareTreeEdge {
  int From = -1;
  int To = -1;
  std::vector<uint64_t> Values;
  std::vector<int> Region;
};

/// A decision tree rooted at block Root whose branches all compare Selector
/// with constants.  Each Interior block holds only pure operations and is
/// entered only from its parent in the tree; its operations can run before
/// the dispatch (in this order) and its branch is folded into it.  Every
/// selector value takes exactly one of Cases or Default.
struct CompareTreeSwitch {
  int Root = -1;
  MedVar Selector;
  std::vector<int> Interior;
  std::vector<CompareTreeEdge> Cases;
  CompareTreeEdge Default;
};

/// Compare trees in \p Med with at least limits::kMinCompareTreeTargets case
/// targets besides the default, whose case targets own their code and leave
/// for one shared follow.  No block is interior to two trees, and no root is
/// interior to another tree.
std::vector<CompareTreeSwitch> findCompareTreeSwitches(const MedFunc &Med);

} // namespace neverd

#endif // NEVERD_LIB_IR_HIGH_LOWER_COMPARETREESWITCH_H
