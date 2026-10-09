//===- InternalNoReturn.cpp - No-return proofs for internal callees -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The LowIR counterpart of the MedIR no-return proof (MedNoReturn.h), run on
/// a callee lifted on demand so that a single-function decompile can cut the
/// path after a call into an internal routine that never returns, such as a
/// wrapper around a bug check.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/InternalNoReturn.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"

#include <vector>

namespace neverd {

InternalNoReturnIndex::InternalNoReturnIndex(
    const BinaryImage &Img, const std::set<va_t> *KnownFuncEntries,
    const libc::NoReturnTargetIndex *NoReturnTargets,
    const detail::AbsoluteRelocationRootIndex *Roots,
    const ExecutableCodeOwnerIndex *CodeOwners)
    : Img(Img), KnownFuncEntries(KnownFuncEntries),
      NoReturnTargets(NoReturnTargets), Roots(Roots), CodeOwners(CodeOwners) {}

bool InternalNoReturnIndex::neverReturns(va_t Target, unsigned Depth) const {
  if (Depth >= limits::kMaxNoReturnProofDepth ||
      !Img.hasExecutableCodeOwnerAt(Target))
    return false;
  const auto Key = std::make_pair(Target, Depth);
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (auto It = Proofs.find(Key); It != Proofs.end())
      return It->second;
  }
  // Two builds may prove the same callee at once; both reach the same
  // answer, so the lock is not held while lifting.
  Decoder Dec;
  bool Proved = false;
  if (Dec.init(Img)) {
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(KnownFuncEntries);
    Builder.setNoReturnTargetIndex(NoReturnTargets);
    Builder.setAbsoluteRelocationRootIndex(Roots);
    Builder.setExecutableCodeOwnerIndex(CodeOwners);
    Builder.setNoReturnCalleeProver(this, Depth + 1);
    const LowFunc Callee =
        Builder.build(Img, Dec, Target, Img.getFunctionNameAt(Target));
    Proved = lowFunctionNeverReturns(Callee, Img.Arch);
  }
  std::lock_guard<std::mutex> Lock(Mutex);
  return Proofs.emplace(Key, Proved).first->second;
}

} // namespace neverd
