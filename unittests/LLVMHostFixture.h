//===- LLVMHostFixture.h - IR fixtures for host Clang drivers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::test {

inline void lowerHostConversionAnnotations(llvm::Instruction &Instruction) {
  auto *Trunc = llvm::dyn_cast<llvm::TruncInst>(&Instruction);
  auto *Extend = llvm::dyn_cast<llvm::ZExtInst>(&Instruction);
  if ((!Trunc || (!Trunc->hasNoUnsignedWrap() && !Trunc->hasNoSignedWrap())) &&
      (!Extend || !Extend->hasNonNeg()))
    return;
  // Older Clang IR parsers cannot spell these conversion guarantees. Keep
  // their poison conditions explicit in the compiler copy, including lanes.
  llvm::IRBuilder<> Builder(&Instruction);
  auto *Input = Instruction.getOperand(0);
  llvm::Value *Value = nullptr, *Allowed = nullptr;
  if (Trunc) {
    Value = Builder.CreateTrunc(Input, Trunc->getType(), "host.trunc");
    if (Trunc->hasNoUnsignedWrap())
      Allowed = Builder.CreateICmpEQ(
          Builder.CreateZExt(Value, Input->getType()), Input);
    if (Trunc->hasNoSignedWrap()) {
      auto *Signed = Builder.CreateICmpEQ(
          Builder.CreateSExt(Value, Input->getType()), Input);
      Allowed = Allowed ? Builder.CreateAnd(Allowed, Signed) : Signed;
    }
  } else {
    Value = Builder.CreateZExt(Input, Extend->getType(), "host.zext");
    Allowed = Builder.CreateICmpSGE(
        Input, llvm::Constant::getNullValue(Input->getType()));
  }
  auto *Checked = Builder.CreateSelect(
      Allowed, Value, llvm::PoisonValue::get(Instruction.getType()));
  if (auto *Replacement = llvm::dyn_cast<llvm::Instruction>(Checked))
    Replacement->takeName(&Instruction);
  Instruction.replaceAllUsesWith(Checked);
  Instruction.eraseFromParent();
}

inline void printHostCompilerFixture(const llvm::Module &Module,
                                     llvm::raw_ostream &Out) {
  // The embedded LLVM can be newer than the host Clang used to compile an
  // executable oracle over source-defined samples. These function/call
  // guarantees have newer IR spellings; omitting them is conservative for
  // those samples. Conversion poison conditions use explicit guards. Semantic
  // proofs keep the unchanged module, including its original range contracts.
  auto Fixture = llvm::CloneModule(Module);
  auto RemoveNewAnnotations = [](auto &FunctionOrCall, unsigned Parameters) {
    FunctionOrCall.removeFnAttr(llvm::Attribute::NoCreateUndefOrPoison);
    FunctionOrCall.removeFnAttr(llvm::Attribute::Memory);
    FunctionOrCall.removeRetAttr(llvm::Attribute::Range);
    for (unsigned I = 0; I != Parameters; ++I) {
      FunctionOrCall.removeParamAttr(I, llvm::Attribute::Captures);
      FunctionOrCall.removeParamAttr(I, llvm::Attribute::Range);
    }
  };
  for (auto &Function : *Fixture) {
    RemoveNewAnnotations(Function, Function.arg_size());
    for (auto &Block : Function)
      for (auto &Instruction : llvm::make_early_inc_range(Block)) {
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          RemoveNewAnnotations(*Call, Call->arg_size());
        lowerHostConversionAnnotations(Instruction);
      }
  }
  Fixture->print(Out, nullptr);
}

inline std::string printHostCompilerFixture(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  printHostCompilerFixture(Module, Out);
  return Text;
}

} // namespace neverd::test
