//===- X86Unsupported.h - Unowned x86 contracts -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep legacy intrinsic identities readable without inventing executable
/// semantics. Specific SHA and bounded RTM operations have separate contracts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86UNSUPPORTED_H
#define NEVERD_IR_INTRINSICS_X86_X86UNSUPPORTED_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

/// These IDs resume at a continuation whose target no named operand carries:
/// RSM restores the whole processor from SMRAM and XBEGIN's abort path is a
/// second, non-sequential exit. Keep their serialized identities, but admit
/// neither an ABI return nor bare assembly.  (The syscall returns SYSRET/
/// SYSEXIT, like far/interrupt returns, DO name their transfer target in a
/// register and are lifted to a terminal state-transfer intrinsic instead.)
inline const char *x86NonlocalControlTransferReason(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Rsm:
    return "x86 nonlocal control transfer requires a complete machine-state "
           "boundary";
  case Intrinsic::Xbegin:
    return "x86 transaction begin requires the original abort continuation "
           "and a complete machine-state checkpoint";
  default:
    return nullptr;
  }
}

inline const char *x86UnsupportedIntrinsicReason(Intrinsic Id) {
  if (const char *Reason = x86NonlocalControlTransferReason(Id))
    return Reason;
  switch (Id) {
  case Intrinsic::BndMk:
  case Intrinsic::BndMov:
  case Intrinsic::BndCl:
  case Intrinsic::BndCu:
  case Intrinsic::BndCn:
  case Intrinsic::BndLdx:
  case Intrinsic::BndStx:
  case Intrinsic::Xacquire:
  case Intrinsic::Xrelease:
  case Intrinsic::ShaGeneric:
    return "unimplemented x86 intrinsic requires an owned semantic contract";
  default:
    return nullptr;
  }
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86UNSUPPORTED_H
