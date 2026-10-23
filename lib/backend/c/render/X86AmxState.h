//===- X86AmxState.h - Architectural AMX source rendering -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared native bank operation rendering for HighC and LLVM-to-C.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86AMXSTATE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86AMXSTATE_H

#include "neverd/ir/high/HighIR.h"

#include <optional>
#include <string>
#include <vector>
namespace llvm {
class CallInst;
}
namespace neverd {
std::string renderX86AmxExpression(Arch A, const HighExpr &E,
                                   const std::vector<std::string> &Arguments,
                                   const std::string &Type);
std::optional<std::string>
renderX86AmxCall(Arch A, const llvm::CallInst &Call,
                 const std::vector<std::string> &Arguments);
} // namespace neverd
#endif // NEVERD_LIB_BACKEND_C_RENDER_X86AMXSTATE_H
