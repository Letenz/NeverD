//===- X64Recovery.h - Native x64 recovery contracts -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64RECOVERY_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64RECOVERY_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/lift/X86Regs.h"

#include <array>

namespace neverd::analysis::x64 {

inline constexpr uint64_t StackPointer = x86reg::RSP;
inline constexpr uint64_t ModelledFlags[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                                             x86reg::ZF, x86reg::SF, x86reg::DF,
                                             x86reg::OF};

struct RawEnvironmentPreparation {
  uint64_t Bytes = 0;
  uint64_t Records = 0;
};

/// Versioned conservative preparation reservation, validated before payload
/// inspection/copy/hash. Includes all options and mixed terminal declarations.
llvm::Expected<RawEnvironmentPreparation>
validateRawEnvironmentDeclarations(const SpecializationOptions &Options);

bool runtimeFlagsIntrinsic(const LowOp &Op);

struct RuntimeTimestampRead {
  std::array<LowOp, 3> Ops;
  std::array<NdVar, 2> PlaceholderOutputs;
};

/// Match the complete canonical x64 RDTSC bundle and bind one caller-supplied
/// sample to its primary result and two instruction-local output placeholders.
/// The original operation remains a runtime read. This scalar transition gives
/// no timestamp, privilege, nonfaulting, source or native-proof certificate.
std::optional<RuntimeTimestampRead>
lowerRuntimeTimestampRead(const SpecializationInstruction &Instruction,
                          NdVar Sample);

bool isRetainedNativeTrap(const SpecializationInstruction &Insn);
bool isCetDisabledProjection(const SpecializationInstruction &Insn);

/// Reset and bind a separate bank fact only for the exact disabled-CET RDSSP
/// projection accepted by the owner above. Never upgrades undefined coverage.
bool bindCetDisabledPreservedState(SpecializationInstruction &Insn);

} // namespace neverd::analysis::x64

#endif
