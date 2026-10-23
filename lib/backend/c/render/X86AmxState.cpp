//===- X86AmxState.cpp - Preserve the native AMX bank in C -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Tile state stays architectural; only row results enter C value storage.
///
//===----------------------------------------------------------------------===//

#include "X86AmxState.h"

#include "neverd/backend/llvm/X86AmxAsm.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

#include <stdexcept>
namespace neverd {
namespace {
std::string gnuText(const std::string &S) {
  std::string R;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S.compare(I, 2, "$$") == 0) {
      R += '$';
      ++I;
      continue;
    }
    if (S.compare(I, 2, "${") == 0 && I + 5 < S.size() && S[I + 3] == ':' &&
        S[I + 5] == '}') {
      R += '%';
      R += S[I + 4];
      R += S[I + 2];
      I += 5;
      continue;
    }
    switch (S[I]) {
    case '$':
      R += '%';
      break;
    case '%':
      R += "%%";
      break;
    case '{':
      R += "%{";
      break;
    case '}':
      R += "%}";
      break;
    case '\n':
      R += "\\n";
      break;
    case '\t':
      R += "\\t";
      break;
    default:
      R += S[I];
      break;
    }
  }
  return R;
}
std::string render(const X86AmxAsm &P, std::vector<std::string> Inputs) {
  if (Inputs.size() != P.InputBytes.size())
    throw std::invalid_argument("invalid AMX assembly argument count");
  std::string Text;
  for (unsigned I = 0; I < Inputs.size(); ++I)
    if (P.InputConstraints[I] == "{r16}") {
      Text += "register uint32_t __neverd_amx_selector __asm__(\"r16\") = "
              "(uint32_t)(" +
              Inputs[I] + "); ";
      Inputs[I] = "__neverd_amx_selector";
    }
  Text += "__asm__ volatile(\"" + gnuText(P.Text) + "\" : : ";
  for (unsigned I = 0; I < Inputs.size(); ++I) {
    if (I)
      Text += ", ";
    const auto C =
        P.InputConstraints[I] == "{r16}" ? "r" : P.InputConstraints[I];
    Text += "\"" + C + "\"((uint" + std::to_string(P.InputBytes[I] * 8) +
            "_t)(" + Inputs[I] + "))";
  }
  Text += " : ";
  for (unsigned I = 0; I < P.Clobbers.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + P.Clobbers[I] + "\"";
  }
  return Text + ");";
}
} // namespace
std::string renderX86AmxExpression(Arch A, const HighExpr &E,
                                   const std::vector<std::string> &Args,
                                   const std::string &Type) {
  X86AmxShape S;
  S.TargetArch = A;
  S.ValidId = E.IntrinsicId == Intrinsic::X86AMXState &&
              E.Kind == ExprKind::Call && E.IntrinsicOutputs.empty() &&
              !E.IsIndirectCall && !E.IndirectTarget && !E.SourceCallHint &&
              !E.DoesNotReturn && E.Operands.size() == Args.size();
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  S.MemoryOrdering = E.MemoryOrdering;
  S.WritableOutput = E.Type && E.Type->Kind == NdTypeKind::Int;
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto *V = E.Operands[I].get();
    S.InputSizes[I] = V && V->Type ? V->Type->Size : 0;
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
    if (!V || V->Kind != ExprKind::Const || !V->Type ||
        V->Type->Kind != NdTypeKind::Int)
      continue;
    if (I == 0)
      S.EmptyBase = V->ConstVal == 0;
    if (I == 1 && V->Type->Size == 4 && V->ConstVal <= UINT32_MAX)
      S.Descriptor = V->ConstVal;
    if (I == 2)
      S.RowImmediate = V->ConstVal;
  }
  if (A != Arch::X64 || !x86AmxShapeIsValid(S))
    throw std::invalid_argument(
        "invalid HighC AMX architectural bank contract");
  const auto K = x86AmxOperation(*S.Descriptor);
  const auto P = x86AmxAsm(*S.Descriptor, S.MemoryAddressSpace);
  std::vector<std::string> Inputs;
  std::string Text = "({ ";
  if (P->Memory) {
    Inputs.push_back(Args[0]);
    if (x86AmxIsTileMemory(K))
      Inputs.push_back(Args[2]);
  } else if (P->Row) {
    Text += Type + " __neverd_amx_row; ";
    Inputs = {"&__neverd_amx_row", Args[2]};
  }
  Text += render(*P, Inputs);
  if (P->Row)
    Text += " __neverd_amx_row;";
  return Text + " })";
}
std::optional<std::string>
renderX86AmxCall(Arch A, const llvm::CallInst &Call,
                 const std::vector<std::string> &Args) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm ||
      !llvm::StringRef(Asm->getAsmString()).starts_with(X86AmxAsmMarker))
    return {};
  auto Header = llvm::StringRef(Asm->getAsmString())
                    .drop_front(llvm::StringRef(X86AmxAsmMarker).size())
                    .split('\n')
                    .first;
  auto Fields = Header.split(' ');
  uint32_t Descriptor;
  unsigned Space;
  if (A != Arch::X64 || Fields.first.getAsInteger(10, Descriptor) ||
      Fields.second.getAsInteger(10, Space) || Space > UINT8_MAX)
    throw std::invalid_argument("invalid AMX native descriptor");
  const auto P =
      x86AmxAsm(Descriptor, static_cast<NdMemoryAddressSpace>(Space));
  if (!P || P->Text != Asm->getAsmString() ||
      P->Constraints != Asm->getConstraintString() || !Asm->hasSideEffects() ||
      Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      !Call.getType()->isVoidTy() || Call.arg_size() != P->InputBytes.size() ||
      Args.size() != Call.arg_size() || Call.getNumOperandBundles() ||
      Call.onlyReadsMemory() || Call.onlyWritesMemory() ||
      Call.doesNotReturn() || Call.getFunctionType()->isVarArg())
    throw std::invalid_argument("invalid AMX native assembly effects");
  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    const auto *T = Call.getArgOperand(I)->getType();
    if (P->Row && I == 0) {
      if (!T->isPointerTy() || T->getPointerAddressSpace())
        throw std::invalid_argument("invalid AMX row storage");
    } else if (!T->isIntegerTy(P->InputBytes[I] * 8))
      throw std::invalid_argument("invalid AMX scalar input");
  }
  if (Descriptor & X86AmxImmediateRow) {
    const auto *C = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
    if (!C || C->getZExtValue() > UINT8_MAX)
      throw std::invalid_argument("invalid AMX row immediate");
  }
  // Scope the fixed r16 binding if more than one row instruction is emitted.
  return "{ " + render(*P, Args) + " }\n";
}
} // namespace neverd
