//===- MedLLVMRegistrationSEHJumpState.cpp - Resumed source states -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Projects the last logical callback state after native nonlocal unwind.

#include "MedLLVMRegistrationSEHJump.h"

namespace neverd::x86_registration {

llvm::Value *emitSEHJumpState(const X86RegistrationSEHJumpCall &Source,
                              llvm::CallInst &Call, llvm::Value &State,
                              llvm::IRBuilder<> &Builder) {
  auto *Initial = Builder.getInt32(Source.Capture->Level);
  if (Source.ResumeLevel == Source.Capture->Level)
    return Initial;
  llvm::Value *Resumed = nullptr;
  if (Source.ResumeLevel) {
    Resumed = Builder.getInt32(*Source.ResumeLevel);
  } else {
    auto *Current = Builder.CreateLoad(Builder.getInt32Ty(), &State);
    Current->setAlignment(llvm::Align(1));
    Current->setVolatile(true);
    Resumed = Current;
    // Compare every key with the original read, never a preceding result.
    // The shared source binder proves this finite projection covers all paths.
    for (const auto &[From, To] : Source.ResumeStateMap)
      Resumed = Builder.CreateSelect(
          Builder.CreateICmpEQ(Current, Builder.getInt32(From)),
          Builder.getInt32(To), Resumed, "registration.jump.unwound");
  }
  return Builder.CreateSelect(Builder.CreateICmpEQ(&Call, Builder.getInt32(0)),
                              Initial, Resumed, "registration.jump.state");
}

} // namespace neverd::x86_registration
