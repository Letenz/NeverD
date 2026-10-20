//===- LLVMCallContract.h - resolved call signature validation ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H
#define NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace llvm {
class CallBase;
class Function;
class IRBuilderBase;
class Module;
class Value;
} // namespace llvm

namespace neverd {
class LLVMSourceMap;
/// Record independent register banks before argument values lose their
/// physical register identities to propagation or LLVM optimization.
void setLLVMArgumentBanks(llvm::Function &Function, unsigned Integers,
                          unsigned Floating);
/// A call also records where its stack values were stored: each one's byte
/// offset from the first stack argument, with the ABI's stack slot size.
/// Without offsets its stack values are one contiguous run of bytes.
void setLLVMArgumentBanks(llvm::CallBase &Call, unsigned Integers,
                          unsigned Floating,
                          llvm::ArrayRef<int64_t> StackOffsets = {},
                          unsigned SlotBytes = 0);
/// Bind each recovered callee parameter to its register bank or stack slot.
/// A missing bank value fails; surplus values never shift another bank.
struct LLVMArgumentBinding {
  std::vector<llvm::Value *> Values;
  /// Original argument index, or -1 for a packed/sliced stack value whose
  /// source pointer interpretation and parameter attributes cannot be reused.
  std::vector<int> SourceIndices;
};
/// A stack value placed by \p StackOffsets owns the bytes up to the end of
/// its last slot, or to the next value: padding a narrower store leaves.
std::optional<LLVMArgumentBinding> bindResolvedLLVMArguments(
    const llvm::Function &Target, llvm::ArrayRef<llvm::Value *> Values,
    unsigned Integers, unsigned Floating, llvm::IRBuilderBase &Builder,
    llvm::ArrayRef<int64_t> StackOffsets = {}, unsigned SlotBytes = 0);
/// Reconcile calls whose exact immutable target becomes known after emission.
/// Drop surplus fixed arguments and adapt scalar integer/pointer carriers.
/// Missing arguments, incompatible carriers and musttail changes fail clearly.
bool normalizeResolvedLLVMCalls(llvm::Module &Module,
                                LLVMSourceMap *Sources = nullptr,
                                bool AnalystSourceProjection = false);
/// Check identities after deferred import placeholders have been promoted.
/// Opaque pointers otherwise let a mismatched direct call pass LLVM's verifier.
/// Check both the type and calling convention after optimization and linking.
/// An inconsistent resolved target emits a diagnostic and returns false.
bool validateResolvedLLVMCallSignatures(llvm::Module &Module,
                                        bool AnalystSourceProjection = false);
} // namespace neverd

#endif
