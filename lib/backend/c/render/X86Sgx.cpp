//===- X86Sgx.cpp - SGX source completion -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// No helper calls or assumed-success flags: emit the owned native assembly.
//===----------------------------------------------------------------------===//

#include "X86Sgx.h"

#include "neverd/backend/llvm/X86SgxAsm.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace neverd {
namespace {

std::string wordType(const X86SgxAsm &P) {
  return "uint" + std::to_string(P.WordBytes * 8) + "_t";
}

std::string renderAsm(const X86SgxAsm &P,
                      const std::array<std::string, 3> &Outputs,
                      const std::vector<std::string> &Arguments) {
  std::string Text;
  for (const char C : P.Text)
    Text += C == '%'    ? "%%"
            : C == '$'  ? "%"
            : C == '\n' ? "\\n"
            : C == '\t' ? "\\t"
                        : std::string(1, C);
  auto Constraint = [](std::string C) {
    for (auto [Fixed, Letter] :
         {std::pair{"{ax}", "a"}, {"{bx}", "b"}, {"{cx}", "c"}, {"{dx}", "d"}})
      if (const auto At = C.find(Fixed); At != std::string::npos)
        C.replace(At, 4, Letter);
    return C;
  };
  Text = "__asm__ volatile(\"" + Text + "\" : ";
  for (unsigned I = 0; I < P.OutputConstraints.size(); ++I) {
    if (I)
      Text += ", ";
    Text +=
        "\"" + Constraint(P.OutputConstraints[I]) + "\"(" + Outputs[I] + ")";
  }
  Text += " : ";
  for (unsigned I = 0; I < P.InputConstraints.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + Constraint(P.InputConstraints[I]) + "\"((" + wordType(P) +
            ")(" + Arguments[I] + "))";
  }
  return Text + " : \"cc\", \"memory\"); ";
}

const std::array<std::string, 3> Locals = {"__neverd_sgx_ax", "__neverd_sgx_bx",
                                           "__neverd_sgx_flags"};

std::string declarations(const X86SgxAsm &P) {
  return wordType(P) + " " + Locals[0] + ", " + Locals[1] + "; uint16_t " +
         Locals[2] + "; ";
}

} // namespace

std::string
renderX86SgxExpression(Arch A, const HighExpr &E,
                       const std::function<std::string(const HighExpr &)> &Expr,
                       const std::function<std::string(const MedVar &)> &Var,
                       const std::function<bool(const MedVar &)> &Alive) {
  X86SgxShape S;
  S.TargetArch = A;
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.ValidId = E.Kind == ExprKind::Call && !E.IsIndirectCall &&
              !E.IndirectTarget && !E.SourceCallHint && !E.DoesNotReturn &&
              E.IntrinsicOutputs.size() == X86SgxAuxiliaryOutputs;
  S.WritableOutput = E.Type && (E.Type->Kind == NdTypeKind::Int ||
                                E.Type->Kind == NdTypeKind::Ptr);
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.InputSizes[I] = V && V->Type ? V->Type->Size : 0;
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
  }
  if (!x86SgxShapeIsValid(E.IntrinsicId, S))
    throw std::invalid_argument("invalid HighC SGX completion contract");
  const auto P = x86SgxAsm(E.IntrinsicId, A);
  std::vector<std::string> Arguments;
  for (const auto &V : E.Operands)
    Arguments.push_back(Expr(*V));
  std::string Text =
      "({ " + declarations(*P) + renderAsm(*P, Locals, Arguments);
  for (unsigned I = 0; I < X86SgxAuxiliaryOutputs; ++I) {
    const auto &V = E.IntrinsicOutputs[I];
    if (V.Kind != MedVar::Temp || V.Id < 0 ||
        V.Size != (I == 0 ? P->WordBytes : X86SgxFlagBytes))
      throw std::invalid_argument("invalid HighC SGX auxiliary result");
    if (Alive(V))
      Text += Var(V) + " = " + Locals[I + 1] + "; ";
  }
  return Text + Locals[0] + "; })";
}

std::optional<std::string>
renderX86SgxCall(Arch A, const llvm::CallInst &Call, const std::string &Result,
                 bool ResultLive, const std::vector<std::string> &Arguments) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return {};
  const auto Mnemonic = llvm::StringRef(Asm->getAsmString()).split('\n').first;
  const auto Id = Mnemonic == "encls"   ? Intrinsic::X86Encls
                  : Mnemonic == "enclv" ? Intrinsic::X86Enclv
                                        : Intrinsic::None;
  if (Id == Intrinsic::None)
    return {};
  const auto P = x86SgxAsm(Id, A);
  const auto *R = llvm::dyn_cast<llvm::StructType>(Call.getType());
  bool Valid =
      P && Asm->getAsmString() == P->Text &&
      Asm->getConstraintString() == P->Constraints && Asm->hasSideEffects() &&
      !Asm->isAlignStack() && !Asm->canThrow() &&
      Asm->getDialect() == llvm::InlineAsm::AD_ATT &&
      !Call.getNumOperandBundles() && !Call.onlyReadsMemory() &&
      !Call.onlyWritesMemory() && !Call.doesNotReturn() &&
      !Call.getFunctionType()->isVarArg() &&
      Call.arg_size() == P->InputConstraints.size() &&
      Arguments.size() == Call.arg_size() && R && R->isLiteral() &&
      !R->isPacked() && R->getNumElements() == P->OutputConstraints.size();
  if (Valid) {
    Valid &= R->getElementType(0)->isIntegerTy(8 * P->WordBytes) &&
             R->getElementType(1)->isIntegerTy(8 * P->WordBytes) &&
             R->getElementType(2)->isIntegerTy(8 * X86SgxFlagBytes);
    for (const auto &V : Call.args())
      Valid &= V->getType()->isIntegerTy(8 * P->WordBytes);
  }
  if (!Valid || (ResultLive && Result.empty()))
    throw std::invalid_argument("unsupported SGX inline assembly contract");
  if (ResultLive)
    return renderAsm(
               *P,
               {Result + ".field_0", Result + ".field_1", Result + ".field_2"},
               Arguments) +
           '\n';
  return "{ " + declarations(*P) + renderAsm(*P, Locals, Arguments) + "}\n";
}

} // namespace neverd
