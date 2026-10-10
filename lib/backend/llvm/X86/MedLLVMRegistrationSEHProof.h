//===- MedLLVMRegistrationSEHProof.h - PE32 SEH preflight -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHPROOF_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHPROOF_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace llvm {
class BasicBlock;
class CallInst;
class Function;
class Instruction;
} // namespace llvm

namespace neverd::x86_registration {

/// The common LowIR provenance, closed over synthetic MedIR copies and PHIs.
struct SEHFrameValues {
  using Key = std::tuple<MedVar::VarKind, int, int>;
  std::set<Key> Values;
  bool contains(const MedVar &V) const {
    return !V.isConst() && Values.count({V.Kind, V.Id, V.SSAVer});
  }
};
std::optional<SEHFrameValues> getPrivateSEHFrameValues(
    const MedFunc &Func, const RegistrationStateAnalysis &States,
    llvm::function_ref<bool(const MedVar &, uint16_t)> IsPrivateStore);

std::optional<std::set<llvm::Instruction *>> getCheckedSEHChainInstructions(
    llvm::Function &Parent, const RegistrationStateAnalysis &States,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationChainIR);

struct SourceOperation {
  llvm::Instruction *Instruction;
  const MedOp *Operation;
  int Block;
  uint8_t Kind;
};
struct SEHSourceOperations {
  std::vector<SourceOperation> Operations;
  std::set<llvm::CallInst *> CookieCheckCalls;
};
std::optional<SEHSourceOperations> getCheckedSEHSourceOperations(
    const MedFunc &Func,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationMemoryIR,
    const std::map<const llvm::CallInst *, va_t> &CallSiteAddrs);

bool collectDeadFinallyResult(llvm::CallInst &Call,
                              std::vector<llvm::Instruction *> &Dead);

} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHPROOF_H
