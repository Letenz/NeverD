//===- MedCallConvention.h - Per-convention call argument rules -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The facts of each calling convention that call-argument recovery relies
/// on: how a direct callee's entry-read summary, an indirect-call dispatcher
/// or the setup writes before an indirect call become register arguments,
/// and where the arguments a call passes live.  Each convention is one table
/// entry defined in its own file (MedCallConventionWin64.cpp,
/// MedCallConventionSysV.cpp, ...); generic recovery asks only for the entry
/// that matches the target and tests the facts it needs, so supporting
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
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {

struct CallArgumentConvention {
  Arch TheArch = Arch::Unknown;
  /// The image format, or Unknown for every format of the architecture.
  BinaryFormat Format = BinaryFormat::Unknown;
  /// A call to a summarized direct callee passes the argument registers the
  /// callee reads at entry (LowToMed publishes them as the CALL's inputs).
  bool RegisterArgumentsFromCalleeSummary = false;
  /// An import whose libc prototype is fixed reads exactly its argument
  /// registers, so its stub and the slot the loader binds have that summary,
  /// and an indirect call through the slot passes them as a call to the stub
  /// does.
  bool ImportArgumentsFromPrototype = false;
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
  /// An integer register that carries no argument and that a call need not
  /// preserve holds no defined value at entry (RAX beyond a variadic
  /// callee's AL, R10, R11), so a store of that incoming value before a call
  /// passes nothing: an alignment `push rax` is no stack argument.
  bool UndefinedIncomingScratchRegisters = false;
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
  /// Only a directly called function of the image takes register arguments
  /// (the compiler's regparm convention for internal functions).  An
  /// imported or indirectly called function takes every argument on the
  /// stack, so a parameter register live at such a call is scratch.
  bool RegparmOnlyForInternalCalls = false;
  /// The first stack argument directly follows the last register argument
  /// the call uses, rather than every register position.
  bool StackArgumentsFollowUsedRegisters = false;
  /// Every variadic argument is passed on the stack, right after the fixed
  /// arguments, even while argument registers remain.
  bool VariadicArgumentsOnStack = false;
  /// A 64-bit argument takes an even-odd register pair or an 8-byte aligned
  /// stack slot, so the 4-byte lane before it can be unused padding.
  bool PairAlignedWideArguments = false;
  /// A variadic function takes every argument, fixed or variadic, on the
  /// stack: it has no register save area, and its named parameters are all
  /// stack parameters.
  bool StackOnlyVariadicCallees = false;
  /// A function's register parameters are the argument registers whose
  /// incoming bytes it reads: those its entry-read summary lists when it has
  /// one, since every caller passes exactly those, else those whose incoming
  /// value reaches a use (a call to a summarized callee reads its arguments).
  bool ParametersFromIncomingReads = false;
  /// Register arguments fill the argument registers in order with no gap
  /// (regparm), so a live register after an unread one is not an argument.
  bool RegisterArgumentsFillInOrder = false;
  /// The argument count a platform prototype gives a named function (the
  /// WDK's, for Windows kernel routines), or nullopt.
  std::optional<size_t> (*PrototypeArgCount)(llvm::StringRef Name) = nullptr;
};

/// The calling convention MedIR records for code of \p A in a \p F image, or
/// nullopt where it leaves the function's convention as detected later
/// (i386 cdecl).
std::optional<CallingConv> callingConventionOf(Arch A, BinaryFormat F);

/// The convention of code for \p A in a \p F image, or nullptr when NeverD
/// has no entry for it.  An entry for that exact format wins over one for
/// every format of the architecture.
const CallArgumentConvention *callArgumentConvention(Arch A, BinaryFormat F);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLCONVENTION_H
