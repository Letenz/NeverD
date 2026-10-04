//===- CallRegisterEffects.h - Callee GPR write summaries ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which general-purpose registers a direct callee may write.
///
/// The ABI lets a callee clobber every volatile register, but a linker with
/// whole-program register allocation (MSVC /LTCG, for example) relies on the
/// callee's actual body: a caller keeps a value in RDX across a call to a
/// helper that never touches RDX.  Treating that call as an ABI clobber loses
/// the value.  This summary is the one place that answers "may the call
/// change this register", from the lifted callee bodies and their callees.
///
/// A summary exists only when the whole call tree is known: every reached
/// instruction lifted, every indirect branch resolved, and every callee is a
/// lifted function with its own summary.  Anything else keeps the ABI answer.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_CALLREGISTEREFFECTS_H
#define NEVERD_IR_LOW_CALLREGISTEREFFECTS_H

#include "neverd/Common.h"
#include "neverd/ir/low/LowIR.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

namespace libc {
class NoReturnTargetIndex;
} // namespace libc

struct BinaryImage;

/// Bit I names the x86-64 GPR whose 8-byte slot starts at register offset
/// 8 * I (RAX = bit 0 ... R15 = bit 15).  A write to any byte of a slot
/// (AL, AH, EAX, ...) sets the whole family.
using GPRFamilyMask = uint32_t;

/// In a may-write mask, a write to the x86-64 floating-point return register
/// (any view of XMM0).  A callee that leaves RAX alone may still return a
/// value there.
constexpr GPRFamilyMask kFPReturnWriteBit = GPRFamilyMask(1) << 16;

/// Per GPR family, how many low bytes of the slot are read (0 = none, 1 for
/// CL, 2 for CX or CH, 4 for ECX, 8 for RCX).
using GPRReadWidths = std::array<uint8_t, 16>;

/// The GPR family of a register slice, or nullopt when \p RegOff is not one
/// of the sixteen x86-64 GPRs this summary tracks.
std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff);

/// One straight-line step of a function, for register liveness.
struct RegisterStep {
  GPRReadWidths Reads{};
  /// Families this step fully redefines (a 32- or 64-bit write).
  GPRFamilyMask Kills = 0;
  /// Low bytes of a family this step writes without redefining the rest (1
  /// for DL, 2 for DX). A later read no wider than that is satisfied here.
  GPRReadWidths LowWrites{};
  /// A direct call or branch into another function's entry.
  va_t Callee = InvalidVA;
  /// \p Callee is entered by a tail call (a branch into its entry or a
  /// rewritten tail jump), so it receives this function's registers.
  bool TailCallee = false;
  /// Control leaves for code that no summary describes: an import tail
  /// call or an indirect tail jump.
  bool UnknownTailCall = false;
  /// An indirect or import call that returns.
  bool UnknownCall = false;
  /// Nothing after this step executes (a call that does not return).
  bool Exits = false;
};

struct RegisterBlock {
  std::vector<RegisterStep> Steps;
  std::vector<size_t> Succs;
};

/// Direct call targets of \p F (including rewritten tail calls), and whether
/// \p F has an effect this summary cannot describe.
struct LocalRegisterEffect {
  GPRFamilyMask Writes = 0;
  std::set<va_t> Callees;
  /// Direct targets of calls proved never to return that the name list does
  /// not know.  Their writes reach no caller, so they stay out of Callees,
  /// but their summaries still give those calls their parameters.
  std::set<va_t> NoReturnCallees;
  /// Some effect escapes the may-write summary (an unknown call, an
  /// incomplete lift).
  bool Unknown = false;
  /// The body itself is not fully known, so neither summary exists.
  bool Incomplete = false;
  /// Control leaves for code no summary describes (an import or indirect tail
  /// call) while this function's argument registers may still be the
  /// caller's; which of them that code reads is unknown.
  bool UnknownEntryReads = false;
  /// Liveness skeleton; block 0 is the entry.
  std::vector<RegisterBlock> Blocks;
  /// Positional arguments implied by the incoming stack-argument slots the
  /// body reads (0 when it reads none). Meaningful only without
  /// UnknownStackReads.
  int StackArgs = 0;
  /// The body may read incoming stack arguments that this summary cannot
  /// bound: the stack pointer is lost, a pointer into the incoming arguments
  /// escapes, or control leaves for code no summary describes.
  bool UnknownStackReads = false;
  /// Callees entered by a tail call at the entry stack pointer; they read
  /// this function's incoming stack arguments as their own.
  std::set<va_t> StackTailCallees;
  /// A Win64 variadic function (`f(fmt, ...)`): the register position of its
  /// first variadic argument, else -1.  Its prologue spills that register and
  /// every later argument register to their home slots and hands a pointer to
  /// the first of those slots on as the va_list.  Those spills are not
  /// argument reads: a caller passes exactly the variadic arguments it sets.
  int VariadicFrom = -1;
};

/// Summarize \p F.  A call into a function \p NoReturnTargets names as
/// never returning (a flagged call, or a tail call the LowIR contract keeps
/// unflagged) ends its path: nothing it writes reaches a caller.
LocalRegisterEffect
localRegisterEffect(const BinaryImage &Img, const LowFunc &F,
                    const libc::NoReturnTargetIndex *NoReturnTargets = nullptr);

struct CallRegisterSummaries {
  /// Families a call may change, for functions whose whole call tree is
  /// known.
  std::map<va_t, GPRFamilyMask> MayWrite;
  /// Bytes of each family read before being written on some path from
  /// entry, including through callees.  A pass-through argument counts; a
  /// register a nested call merely receives does not.
  std::map<va_t, GPRReadWidths> EntryReads;
  /// Positional arguments implied by the incoming stack slots a function
  /// reads, including through tail calls (0 when none), for functions whose
  /// stack reads are bounded.
  std::map<va_t, int> EntryStackArgs;
  /// Register position of the first variadic argument of each variadic
  /// function (LocalRegisterEffect::VariadicFrom).
  std::map<va_t, int> VariadicFrom;
};

/// Solve may-write and entry-read GPR sets over \p Funcs (entry -> local
/// effect).  A callee missing from \p Funcs, or reaching an unknown effect,
/// has no may-write summary.  An unknown call is taken to clobber
/// \p VolatileFamilies.  A function that tail-calls code no summary
/// describes (an import, an indirect target, or a function without an
/// entry-read summary) has no entry-read summary either.
///
/// A call to one of \p DispatchThunks (an indirect-call dispatcher such as
/// MSVC's `_guard_dispatch_icall`, which jumps to RAX with the caller's
/// argument registers) is an indirect call whose target is unknown: it
/// clobbers \p VolatileFamilies and does not make the caller read its own
/// incoming argument registers.  The call site's arguments are the registers
/// the caller itself set.  A function in \p FixedEntryReads (a documented
/// prototype) reads exactly those bytes.
CallRegisterSummaries solveCallRegisterEffects(
    const std::map<va_t, LocalRegisterEffect> &Funcs,
    GPRFamilyMask VolatileFamilies, GPRFamilyMask ArgumentFamilies,
    const std::set<va_t> &DispatchThunks = {},
    const std::map<va_t, GPRReadWidths> &FixedEntryReads = {});

} // namespace neverd

#endif // NEVERD_IR_LOW_CALLREGISTEREFFECTS_H
