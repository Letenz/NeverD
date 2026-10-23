//===- X86Unsupported.h - Unowned x86 contracts -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep legacy intrinsic identities readable without inventing executable
/// semantics. Specific SHA and RTM instructions have separate owned contracts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86UNSUPPORTED_H
#define NEVERD_IR_INTRINSICS_X86UNSUPPORTED_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

inline const char *x86UnsupportedIntrinsicReason(Intrinsic Id) {
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

#endif // NEVERD_IR_INTRINSICS_X86UNSUPPORTED_H
