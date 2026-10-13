//===- ItaniumHandlers.h - Catch bodies from Itanium landing pads -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An Itanium region's clauses all enter one landing pad, ordinary code of
/// the function that tests the selector the personality left it and runs
/// the clause it names.  A C++ view spells each clause as `catch` with its
/// own body: the pad's code as it runs for that clause's selector.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_ITANIUMHANDLERS_H
#define NEVERD_IR_HIGH_ITANIUMHANDLERS_H

#include "neverd/ir/high/HighIR.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {

/// Fill the clause bodies of each C++ Itanium region of \p Func from its
/// landing pad, and remove the pad code that only those bodies run.  A clause
/// body starts with the pad's work before it tests the selector (destructors
/// of the region's objects), then the handler, whose `__cxa_begin_catch`
/// keeps no argument: the caught object is the clause's parameter.  The
/// `__cxa_end_catch` calls that leave the handler are gone, as C++ leaves it
/// at the end of a catch.  A region whose pad this cannot follow exactly
/// keeps its clauses empty and its pad in place.
void attachItaniumHandlerBodies(HighFunc &Func);

/// Whether \p Call calls the Itanium C++ runtime routine \p Routine, such as
/// `__cxa_begin_catch`, under any spelling a symbol table gives it.
bool isItaniumRuntimeCall(const HighExpr &Call, llvm::StringRef Routine);

} // namespace neverd

#endif // NEVERD_IR_HIGH_ITANIUMHANDLERS_H
