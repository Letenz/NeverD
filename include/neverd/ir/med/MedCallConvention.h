//===- MedCallConvention.h - Per-convention call argument rules -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// How each calling convention turns what NeverD knows about a callee into
/// the register arguments a call site passes: a direct callee's entry-read
/// summary, an indirect-call dispatcher, or the setup writes before an
/// indirect call.  Each convention is one table entry defined in its own
/// file (MedCallConventionWin64.cpp, MedCallConventionSysV.cpp); generic
/// lowering asks only for the entry that matches the target, so supporting
/// another convention adds a file and a registry line.
///
/// The argument registers themselves are TargetRegInfo::integerParamRegs()
/// of the convention's format.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLCONVENTION_H
#define NEVERD_IR_MED_MEDCALLCONVENTION_H

#include "neverd/Common.h"
#include "neverd/ir/low/CallRegisterEffects.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct CallArgumentConvention {
  Arch TheArch = Arch::Unknown;
  BinaryFormat Format = BinaryFormat::Unknown;
  /// The callee's entry-read summary is not its parameter list, so the call
  /// passes no summarized arguments (a System V variadic prologue spills
  /// every argument register to its save area).  Null when every summary is.
  bool (*SummaryListsNoParameters)(const GPRReadWidths &Reads) = nullptr;
  /// A call to an indirect-call dispatcher (Control Flow Guard) calls the
  /// function in this register with the argument registers set on every path
  /// to the call.  Empty when the convention has no such dispatcher.
  std::optional<uint64_t> DispatcherTargetRegister;
  /// A variadic callee's summary names the position of its first variadic
  /// argument (Win64 home-slot spills), and the call also passes the argument
  /// registers set on every path from there.
  bool VariadicFromSummary = false;
  /// The incoming stack-argument summary counts this convention's positions.
  bool StackArgumentSummary = false;
  /// An indirect call whose own block sets no argument register takes the
  /// consecutive setup writes of the block before it, as IDA does.
  bool IndirectCallsTakePrecedingSetup = false;
  /// Every argument takes the next position whatever its class: register
  /// position K is the Kth integer or the Kth vector argument register, and
  /// the stack positions follow all the register ones.
  bool PositionalArgumentSlots = false;
  /// The caller reserves its outgoing argument area once, with a home slot
  /// for each register position.  A stack argument is a store at a fixed
  /// offset from the stack pointer anywhere before the call, also at a fixed
  /// offset from the entry stack pointer, and the callee may read every slot
  /// below the last one passed.
  bool ReservedOutgoingArea = false;
};

/// The convention of code for \p A in a \p F image, or nullptr when NeverD
/// derives no register arguments from callee summaries there.
const CallArgumentConvention *callArgumentConvention(Arch A, BinaryFormat F);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLCONVENTION_H
