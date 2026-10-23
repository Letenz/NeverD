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

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

#define NEVERD_A64_PAUTH_VALUE(Type, Name, Value)                              \
  inline constexpr Type Name = Value;
#define NEVERD_A64_PAUTH_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64PAuth.def"

struct AArch64PAuthOperation {
  Intrinsic Id;
  const char *Mnemonic;
  unsigned Key;
  bool Strip;
  bool Authenticate;

  std::string llvmAsm() const {
    return std::string(Mnemonic) +
           (Strip ? kAArch64PAuthLLVMStripOperands : kAArch64PAuthLLVMOperands);
  }
  const char *llvmConstraints() const {
    return Strip ? kAArch64PAuthLLVMStripConstraints
                 : kAArch64PAuthLLVMConstraints;
  }
};

inline constexpr AArch64PAuthOperation kAArch64PAuthOperations[] = {
#define NEVERD_A64_PAUTH_OPERATION(Id, Insn, Mnemonic, Key, Strip,             \
                                   Authenticate)                               \
  {Intrinsic::Id, Mnemonic, kAArch64PAuthKey##Key, Strip, Authenticate},
#include "neverd/ir/intrinsics/AArch64PAuth.def"
};

inline std::optional<AArch64PAuthOperation>
aarch64PAuthOperation(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_PAUTH_OPERATION(Id, Insn, Mnemonic, Key, Strip,             \
                                   Authenticate)                               \
  case Intrinsic::Id:                                                          \
    return AArch64PAuthOperation{Intrinsic::Id, Mnemonic,                      \
                                 kAArch64PAuthKey##Key, Strip, Authenticate};
#include "neverd/ir/intrinsics/AArch64PAuth.def"
  default:
    return std::nullopt;
  }
}

inline bool aarch64PAuthCodeKeyIsValid(uint64_t Key) {
  return Key == kAArch64PAuthKeyIA || Key == kAArch64PAuthKeyIB;
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
  if (Shape.OutputBytes != kAArch64PAuthPointerBytes ||
      Shape.PointerBytes != kAArch64PAuthPointerBytes)
    return false;
  if (const auto Operation = aarch64PAuthOperation(Id))
    return Shape.NumInputs == (Operation->Strip ? 2 : 3) &&
           (Operation->Strip ||
            Shape.ModifierBytes == kAArch64PAuthPointerBytes);
  return Id == Intrinsic::A64_AuthBranch && Shape.NumInputs == 4 &&
         Shape.ModifierBytes == kAArch64PAuthPointerBytes &&
         Shape.KeyBytes == kAArch64PAuthKeyBytes && Shape.Key &&
         aarch64PAuthCodeKeyIsValid(*Shape.Key);
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64PAUTH_H
