//===- AArch64PAuthC.h - Shared pointer authentication C text ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Both C projections preserve machine PAC/AUT's tied pointer and keep a
/// control-flow authentication observable even when its value is unused.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64PAUTHC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64PAUTHC_H

#include "neverd/ir/intrinsics/AArch64PAuth.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <string>

namespace neverd {

#define NEVERD_A64_PAUTH_C_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64PAuthC.def"

/// The initializer and asm inputs must refer to their enclosing C scope.
/// Grow the name until it occurs in none of the supplied operand expressions.
inline std::string
aarch64PAuthTemporary(llvm::StringRef Stem,
                      llvm::ArrayRef<llvm::StringRef> Inputs) {
  std::string Name = Stem.str();
  while (llvm::any_of(
      Inputs, [&](llvm::StringRef Input) { return Input.contains(Name); }))
    Name += '_';
  return Name;
}

inline std::string aarch64PAuthMachineC(const AArch64PAuthOperation &Operation,
                                        llvm::StringRef Pointer,
                                        llvm::StringRef Modifier = {}) {
  const std::string Name =
      aarch64PAuthTemporary(kAArch64PAuthCPointer, {Pointer, Modifier});
  return "({ " +
         llvm::formatv(kAArch64PAuthCPointerDeclaration, Name, Pointer).str() +
         llvm::formatv(
             kAArch64PAuthCMachineAsm, Operation.Mnemonic,
             Operation.Strip ? kAArch64PAuthCStripOperands
                             : kAArch64PAuthCOperands,
             Name,
             Operation.Strip
                 ? std::string()
                 : llvm::formatv(kAArch64PAuthCModifier, Modifier).str())
             .str() +
         Name + "; })";
}

inline std::string aarch64PAuthAuthenticateC(llvm::StringRef Pointer,
                                             llvm::StringRef Key,
                                             llvm::StringRef Modifier) {
  return llvm::formatv(kAArch64PAuthCAuthenticate,
                       intrinsicCName(Intrinsic::A64_AuthBranch), Pointer, Key,
                       Modifier)
      .str();
}

inline std::string aarch64PAuthGenericSignC(llvm::StringRef Value,
                                            llvm::StringRef Discriminator) {
  const std::string Name =
      aarch64PAuthTemporary(kAArch64PAuthCPointer, {Value, Discriminator});
  return "({ " +
         llvm::formatv(kAArch64PAuthCGenericSign, Name,
                       intrinsicAsmMnemonic(Intrinsic::A64_Pacga), Value,
                       Discriminator)
             .str() +
         Name + "; })";
}

inline std::string aarch64PAuthTargetUseC(llvm::StringRef Target) {
  return llvm::formatv(kAArch64PAuthCTargetUse,
                       kAArch64AuthenticatedTargetUseAsm, Target)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64PAUTHC_H
