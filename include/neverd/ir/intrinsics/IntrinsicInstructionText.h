//===- IntrinsicInstructionText.h - The instruction behind one --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The instruction an intrinsic was lifted from, as a listing spells it,
/// where the intrinsic's C spelling leaves out what the instruction carries:
/// a trap's or a breakpoint's code (`brk #0x3E8`, which `__builtin_trap()`
/// does not say), a barrier's option (`dmb ishld`), and the system register
/// or operation of an AArch64 MRS, MSR or SYS (`dc civac`).  The lifters
/// keep that operand as the intrinsic's first input; the C writers print
/// this text beside the call, or as the instruction an `asm` statement runs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H
#define NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

/// Whether \p Id keeps its instruction's operand as its first input: a trap
/// or breakpoint code, a hypervisor or secure-monitor call number, or a
/// barrier option.
bool intrinsicCarriesInstructionOperand(Intrinsic Id);

/// The instruction \p Id with first input \p Operand was lifted from
/// (`brk #0x3E8`, `dsb sy`, `ud1 eax, dword ptr [eax + 0x15]`); empty for an
/// intrinsic that carries no such operand, or without one.
std::string intrinsicInstructionText(Intrinsic Id,
                                     std::optional<uint64_t> Operand);

/// The option name of a DMB, DSB or ISB option field \p CRm (`ish`), or its
/// immediate (`#0x4`).
std::string armBarrierOptionText(uint64_t CRm);

/// The option field a barrier option \p Text spells (`ish` is 0xB, `#0x4`
/// is 4); none for any other text.
std::optional<uint64_t> armBarrierOptionValue(llvm::StringRef Text);

/// The name, as the listing prints it, of the AArch64 system register an
/// MRS reads, or an MSR writes when \p Write, at \p Encoding: op0, op1, CRn,
/// CRm and op2, as bits [20:5] of the instruction hold them (`cntvct_el0` is
/// 0xDF02).  Its generic name, `s3_3_c15_c2_0`, which every assembler and
/// ACLE's __arm_rsr64 accept, for one without a name.
std::string aarch64SystemRegisterName(uint64_t Encoding, bool Write);

/// The assembly of the AArch64 system instruction \p Id (Mrs, Msr, Dc, Ic,
/// Tlbi or At) the lifter kept the op0:op1:CRn:CRm:op2 \p Field of, with
/// `$0` for its output or else its register operand: `mrs $0, cntvct_el0`,
/// `msr tpidr_el0, $0`, `msr daifset, #0x2`, `dc civac, $0`, `ic iallu`,
/// `sysl $0, #3, c15, c0, #1`.  Empty where \p Field and the operands fit no
/// form of \p Id.
std::string aarch64SystemInstructionAsm(Intrinsic Id, uint64_t Field,
                                        bool HasOutput, bool HasRegister);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICINSTRUCTIONTEXT_H
