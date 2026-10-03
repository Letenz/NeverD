//===- HighFlowOracle.h - Structured flow versus MedIR edges ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A development check that compares structured HighIR control flow with the
/// MedIR CFG it was built from.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHFLOWORACLE_H
#define NEVERD_IR_HIGH_HIGHFLOWORACLE_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {

/// When NEVERD_HIGH_FLOW_ORACLE is set, print one line for \p Func at
/// \p Stage: block pairs its structured flow runs through that no MedIR edge
/// joins, and MedIR edges between blocks it keeps that its flow never takes.
/// "2" also lists each finding.
void reportHighFlowOracle(const HighFunc &Func, const MedFunc &Med,
                          llvm::StringRef Stage);

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHFLOWORACLE_H
