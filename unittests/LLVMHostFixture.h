//===- LLVMHostFixture.h - IR fixtures for host Clang drivers -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::test {

inline void printHostCompilerFixture(const llvm::Module &Module,
                                     llvm::raw_ostream &Out) {
  // The embedded LLVM can be newer than the host Clang used to compile an
  // executable oracle. These three optimization annotations have newer IR
  // spellings. Omitting their guarantees is conservative; retain every
  // instruction, type, ABI attribute, and the module under test unchanged.
  auto Fixture = llvm::CloneModule(Module);
  auto RemoveNewAnnotations = [](auto &FunctionOrCall, unsigned Parameters) {
    FunctionOrCall.removeFnAttr(llvm::Attribute::NoCreateUndefOrPoison);
    FunctionOrCall.removeFnAttr(llvm::Attribute::Memory);
    for (unsigned I = 0; I != Parameters; ++I)
      FunctionOrCall.removeParamAttr(I, llvm::Attribute::Captures);
  };
  for (auto &Function : *Fixture) {
    RemoveNewAnnotations(Function, Function.arg_size());
    for (auto &Block : Function)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
          RemoveNewAnnotations(*Call, Call->arg_size());
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
