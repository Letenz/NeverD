//===- LLVMSourceMap.h - Non-owning LLVM source observations --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCEMAP_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCEMAP_H

#include "neverd/sigs/LibraryRecognition.h"

#include "llvm/IR/ValueHandle.h"

#include <functional>
#include <map>

namespace neverd {

struct LLVMSourceObservation {
  va_t Function = 0;
  sigs::LibraryOccurrence Occurrence;
  /// Tracks replacement and becomes null on deletion. Never retain a naked
  /// instruction pointer across optimization or module replacement.
  llvm::WeakTrackingVH Value;
};

/// Only sidecar state: no metadata or naming changes in the LLVM module.
class LLVMSourceMap {
public:
  std::map<va_t, llvm::WeakTrackingVH> Functions;
  std::vector<LLVMSourceObservation> Observations;

  /// Rebind a copied snapshot through an explicit clone map. Unmapped values
  /// become unknown; a caller publishes it only when publishing that clone.
  void remap(const std::function<llvm::Value *(llvm::Value *)> &Lookup) {
    for (auto &[Entry, Function] : Functions) {
      (void)Entry;
      Function = Function ? Lookup(Function) : nullptr;
    }
    for (auto &Observation : Observations)
      Observation.Value =
          Observation.Value ? Lookup(Observation.Value) : nullptr;
  }
};

} // namespace neverd

#endif
