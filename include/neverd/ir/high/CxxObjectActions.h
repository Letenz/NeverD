//===- CxxObjectActions.h - Destructor calls of FH4 object actions -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An FH4 unwind action may name an object's destructor itself, with the
/// object by its offset from the establisher frame.  A C++ view spells that
/// cleanup as the destructor called on the object.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_CXXOBJECTACTIONS_H
#define NEVERD_IR_HIGH_CXXOBJECTACTIONS_H

#include "neverd/ir/high/HighIR.h"

namespace neverd {

/// Give each empty cleanup clause of \p Func whose action is an object's
/// destructor that destructor's call on the object.  On x64 the establisher
/// frame is the stack pointer after the prologue's allocation, entry SP -
/// FrameSize, as for the catch funclets' parent frame; other targets keep the
/// action as a note, as does an object whose address the function's own code
/// never computes.  PE32 registration frames keep their native relays.
void attachCxxObjectActionCalls(HighFunc &Func, Arch TargetArch);

} // namespace neverd

#endif // NEVERD_IR_HIGH_CXXOBJECTACTIONS_H
