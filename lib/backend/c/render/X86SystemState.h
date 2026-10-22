//===- X86SystemState.h - System instruction C rendering -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared HighC and LLVM C rendering of owned system assembly contracts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86SYSTEMSTATE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86SYSTEMSTATE_H

#include "neverd/backend/llvm/X86SystemStateAsm.h"

#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallInst;
}

namespace neverd {
struct HighExpr;

std::string
renderX86SystemStateExpression(Arch TargetArch, const HighExpr &Expression,
                               const std::vector<std::string> &Arguments,
                               bool MsvcIntrinsics, bool &HasCIntrinsics);

std::optional<std::string>
renderX86SystemStateCall(Arch TargetArch, const llvm::CallInst &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86SYSTEMSTATE_H
