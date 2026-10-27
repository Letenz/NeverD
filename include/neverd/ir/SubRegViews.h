//===- SubRegViews.h - Sub-register entries by narrow view ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The entries of a target's sub-register table that describe one narrow
/// register view, found through the table's index by narrow view.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_SUBREGVIEWS_H
#define NEVERD_IR_SUBREGVIEWS_H

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>

namespace neverd {

/// Calls \p Visit on each entry of \p TRI's SubRegs whose narrow view is
/// (\p NarrowOff, \p NarrowSize), in table order: the wider registers that
/// contain that view.
void forEachSubRegOfNarrowView(
    const TargetRegInfo &TRI, uint64_t NarrowOff, uint16_t NarrowSize,
    llvm::function_ref<void(const SubRegEntry &)> Visit);

} // namespace neverd

#endif // NEVERD_IR_SUBREGVIEWS_H
