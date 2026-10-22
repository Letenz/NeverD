//===- AArch64PAuth.h - Pointer authentication contracts -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Machine pointer operations retain their operands and architectural failure
/// behavior. AUT can produce a poisoned pointer; authenticating a control-flow
/// target additionally requires a valid pointer before transferring control.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64PAUTH_H
#define NEVERD_IR_INTRINSICS_AARCH64PAUTH_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <optional>

namespace neverd {

/// Analysis modules retain the source ABI; native rebuilding must not silently
/// substitute the ordinary arm64 ABI or materialize unsigned fixup targets.
inline constexpr char kArm64eAnalysisModuleFlag[] = "neverd.arm64e-analysis";

/// A side-effecting use keeps authentication live when RETURN consumes the
/// machine target only implicitly. LLVM DCE can discard unused ptrauth.auth.
inline constexpr char kAArch64AuthenticatedTargetUseAsm[] =
    "// neverd authenticated target";

struct AArch64PAuthOperation {
  const char *Mnemonic;
  unsigned Key;
  bool Strip;
  bool Authenticate = false;
};

inline std::optional<AArch64PAuthOperation>
aarch64PAuthOperation(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Pacia:
    return AArch64PAuthOperation{"pacia", 0, false};
  case Intrinsic::Pacib:
    return AArch64PAuthOperation{"pacib", 1, false};
  case Intrinsic::Pacda:
    return AArch64PAuthOperation{"pacda", 2, false};
  case Intrinsic::Pacdb:
    return AArch64PAuthOperation{"pacdb", 3, false};
  case Intrinsic::Autia:
    return AArch64PAuthOperation{"autia", 0, false, true};
  case Intrinsic::Autib:
    return AArch64PAuthOperation{"autib", 1, false, true};
  case Intrinsic::Autda:
    return AArch64PAuthOperation{"autda", 2, false, true};
  case Intrinsic::Autdb:
    return AArch64PAuthOperation{"autdb", 3, false, true};
  case Intrinsic::Xpaci:
    return AArch64PAuthOperation{"xpaci", 0, true};
  case Intrinsic::Xpacd:
    return AArch64PAuthOperation{"xpacd", 2, true};
  default:
    return std::nullopt;
  }
}

/// Valid pointer authentication writes only its explicit output. It may read
/// hidden key state or fault, but cannot clobber call arguments or memory.
struct AArch64PAuthShape {
  unsigned NumInputs = 0;
  unsigned OutputBytes = 0;
  unsigned PointerBytes = 0;
  unsigned ModifierBytes = 0;
  unsigned KeyBytes = 0;
  std::optional<uint64_t> Key;
};

inline bool aarch64PAuthShapeIsValid(Intrinsic Id,
                                     const AArch64PAuthShape &Shape) {
  if (Shape.OutputBytes != 8 || Shape.PointerBytes != 8)
    return false;
  if (const auto Operation = aarch64PAuthOperation(Id))
    return Shape.NumInputs == (Operation->Strip ? 2 : 3) &&
           (Operation->Strip || Shape.ModifierBytes == 8);
  return Id == Intrinsic::A64_AuthBranch && Shape.NumInputs == 4 &&
         Shape.ModifierBytes == 8 && Shape.KeyBytes == 4 && Shape.Key &&
         *Shape.Key <= 1;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64PAUTH_H
