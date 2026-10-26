//===- X86AtomicExchange.h - Memory XCHG C projection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the native transaction without promising pointer alignment.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86ATOMICEXCHANGE_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86ATOMICEXCHANGE_H

#include "neverd/backend/llvm/X86/X86AtomicExchange.h"

#include <optional>
#include <string>

namespace llvm {
class CallInst;
}

namespace neverd {

std::string renderX86AtomicExchange(const X86AtomicExchangePlan &Plan,
                                    const std::string &Address,
                                    const std::string &Value);
std::optional<X86AtomicExchangePlan>
x86AtomicExchangeCallPlan(Arch Architecture, const llvm::CallInst &Call);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86ATOMICEXCHANGE_H
