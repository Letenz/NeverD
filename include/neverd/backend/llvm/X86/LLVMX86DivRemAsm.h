//===- LLVMX86DivRemAsm.h - Native divide result contract -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share the native DIV/IDIV register contract with LLVM-C. Both result
/// registers and the instruction's possible trap survive C projection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86DIVREMASM_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86DIVREMASM_H

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>
#include <string>

namespace neverd {

inline constexpr char X86DivRemAsmConstraints[] =
    "={ax},={dx},{ax},{dx},r,~{flags}";

struct X86DivRemAsm {
  unsigned Bits;
  bool Signed;
};

inline const char *x86DivRemMnemonic(X86DivRemAsm Operation) {
  if (Operation.Bits == 32)
    return Operation.Signed ? "idivl" : "divl";
  if (Operation.Bits == 64)
    return Operation.Signed ? "idivq" : "divq";
  llvm::report_fatal_error("unsupported native x86 divide width");
}

inline std::string x86DivRemAssembly(X86DivRemAsm Operation) {
  return std::string(x86DivRemMnemonic(Operation)) + " $4";
}

inline std::optional<X86DivRemAsm>
classifyX86DivRemAsm(const llvm::CallInst &Call) {
  const auto *Assembly =
      llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Assembly)
    return std::nullopt;
  std::optional<X86DivRemAsm> Operation;
  for (unsigned Bits : {32U, 64U})
    for (bool Signed : {false, true})
      if (Assembly->getAsmString() == x86DivRemAssembly({Bits, Signed}))
        Operation = X86DivRemAsm{Bits, Signed};
  if (!Operation)
    return std::nullopt;
  const auto *Pair = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Assembly->hasSideEffects() || Assembly->isAlignStack() ||
      Assembly->getDialect() != llvm::InlineAsm::AD_ATT ||
      Assembly->getConstraintString() != X86DivRemAsmConstraints || !Pair ||
      Pair->isOpaque() || Pair->getNumElements() != 2 ||
      !Pair->getElementType(0)->isIntegerTy(Operation->Bits) ||
      Pair->getElementType(1) != Pair->getElementType(0) ||
      Call.getFunctionType()->isVarArg() || Call.arg_size() != 3)
    llvm::report_fatal_error("invalid native x86 divide assembly contract");
  for (const auto &Argument : Call.args())
    if (!Argument->getType()->isIntegerTy(Operation->Bits))
      llvm::report_fatal_error("invalid native x86 divide assembly contract");
  return Operation;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86DIVREMASM_H
