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

/// These legacy IDs do not carry the register banks, flags, stack and
/// privilege/SMM state their nonlocal continuation observes. Keep their
/// serialized identities, but admit neither an ABI return nor bare assembly.
inline const char *x86NonlocalControlTransferReason(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Rsm:
  case Intrinsic::Sysexitq:
  case Intrinsic::Sysretq:
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
