//===- InternalNoReturn.h - No-return proofs for callees --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Proves that an internal function never returns, so that a direct call to
/// it ends its block during CFG construction like a call to a routine the
/// name list knows.  Only a proof cuts the path: no path of the callee may
/// reach a return, and every one must reach an architectural trap or a call
/// that is itself known or proved never to return.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_INTERNALNORETURN_H
#define NEVERD_IR_LOW_INTERNALNORETURN_H

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"

#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace neverd {

/// Memoized no-return proofs for the internal functions of one unchanged
/// image, shared by the CFG builds of one pipeline run.  A proof lifts the
/// callee with the given CFG settings and this prover, so the callee's own
/// calls are proved in turn, up to limits::kMaxNoReturnProofDepth; a callee
/// past that depth counts as returning.  The answer for a callee and depth
/// does not depend on which build asked first.
class InternalNoReturnIndex final : public NoReturnCalleeProver {
public:
  /// The pointers are borrowed for the lifetime of the index, as by
  /// CFGBuilder's setters of the same names.
  InternalNoReturnIndex(const BinaryImage &Img,
                        const std::set<va_t> *KnownFuncEntries,
                        const libc::NoReturnTargetIndex *NoReturnTargets,
                        const detail::AbsoluteRelocationRootIndex *Roots,
                        const ExecutableCodeOwnerIndex *CodeOwners);

  bool neverReturns(va_t Target, unsigned Depth) const override;

private:
  const BinaryImage &Img;
  const std::set<va_t> *KnownFuncEntries;
  const libc::NoReturnTargetIndex *NoReturnTargets;
  const detail::AbsoluteRelocationRootIndex *Roots;
  const ExecutableCodeOwnerIndex *CodeOwners;
  mutable std::mutex Mutex;
  mutable std::map<std::pair<va_t, unsigned>, bool> Proofs;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_INTERNALNORETURN_H
