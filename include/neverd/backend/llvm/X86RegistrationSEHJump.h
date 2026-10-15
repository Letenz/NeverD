//===- X86RegistrationSEHJump.h - Source nonlocal jump calls ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHJUMP_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHJUMP_H

#include "neverd/ir/RegistrationState.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>

namespace llvm {
class AllocaInst;
class Function;
class Value;
} // namespace llvm

namespace neverd {
struct MedFunc;
struct MedOp;

namespace x86_registration {
inline constexpr llvm::StringLiteral SEHJumpUnwindName("_seh_longjmp_unwind");
inline constexpr llvm::StringLiteral
    SEHSourceJumpUnwindName("__nd_source_seh_longjmp_unwind");

enum class SEHJumpRole : unsigned {
  Buffer = 0,
  RegisterSnapshot = 1,
  StateRestore = 2,
  ResumeValue = 3,
};
} // namespace x86_registration

struct X86RegistrationSEHJumpCall {
  const RegistrationSEHJumpCapture *Capture = nullptr;
  const RegistrationSEHJumpResume *Resume = nullptr;
  const MedOp *Operation = nullptr;
  int Block = -1;
  int32_t ResumeLevel = -1;
};

/// Bind complete source buffer lifetimes to exact MedIR call occurrences.
/// A native buffer and the generated capture state must be checked separately.
std::optional<std::map<va_t, X86RegistrationSEHJumpCall>>
getX86RegistrationSEHJumpCalls(const MedFunc &Function);

struct X86RegistrationSEHJumpStack {
  const llvm::AllocaInst *Slot = nullptr;
  uint64_t EntrySP = 0;
};

/// Read the actual bounded stack recipe of a previously authenticated callback.
/// Source ownership and the root's scalar use closure are checked separately.
std::optional<X86RegistrationSEHJumpStack>
getX86RegistrationSEHJumpStack(const llvm::Function &Callback);

/// Recover the physical parent frame through the callback's checked ABI.
/// Source ownership and the complete callback protocol are checked separately.
const llvm::Value *
getX86RegistrationSEHJumpParentFrame(const llvm::Function &Callback,
                                     const llvm::Function &Parent);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHJUMP_H
