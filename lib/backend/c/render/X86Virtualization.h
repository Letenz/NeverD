//===- X86Virtualization.h - Shared architectural asm source --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// C rendering and exact LLVM inline-assembly admission for VMX/SVM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86VIRTUALIZATION_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86VIRTUALIZATION_H

#include "neverd/backend/llvm/X86VirtualizationAsm.h"

#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallInst;
}

namespace neverd {
struct HighExpr;

std::string
renderX86VirtualizationExpression(Arch TargetArch, const HighExpr &Expression,
                                  const std::vector<std::string> &Arguments);

std::optional<std::string>
renderX86VirtualizationCall(Arch TargetArch, const llvm::CallInst &Call,
                            const std::string &Result, bool ResultLive,
                            const std::vector<std::string> &Arguments);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86VIRTUALIZATION_H
