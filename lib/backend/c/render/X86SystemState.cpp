//===- X86SystemState.cpp - System instruction C rendering --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep system operands and completion tied to the LLVM assembly contract.
/// Memory remains instruction-owned, including conditional ARPL writes.
//===----------------------------------------------------------------------===//

#include "X86SystemState.h"

#include "neverd/ir/high/HighIR.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

#include <stdexcept>

namespace neverd {
namespace {

std::string unsignedType(uint16_t Bytes) {
  return Bytes == 16 ? "__uint128_t"
                     : "uint" + std::to_string(8 * Bytes) + "_t";
}

std::string gnuConstraint(const std::string &Text) {
  if (Text == "{ax}")
    return "a";
  if (Text == "{dx}")
    return "d";
  return Text;
}

std::string gnuText(const std::string &Text) {
  std::string Result;
  for (char C : Text) {
    if (C == '$')
      Result += '%';
    else if (C == '%')
      Result += "%%";
    else if (C == '\n')
      Result += "\\n";
    else if (C == '\t')
      Result += "\\t";
    else
      Result += C;
  }
  return Result;
}

std::string renderAsm(const X86SystemStateAsm &P,
                      const std::vector<std::string> &Outputs,
                      const std::vector<std::string> &Inputs) {
  if (Outputs.size() != P.OutputBytes.size() ||
      Inputs.size() != P.InputBytes.size())
    throw std::invalid_argument("invalid system assembly operand count");
  std::string Text = "__asm__ volatile(\"" + gnuText(P.Text) + "\" : ";
  for (size_t I = 0; I < Outputs.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + P.OutputConstraints[I] + "\"(" + Outputs[I] + ")";
  }
  Text += " : ";
  for (size_t I = 0; I < Inputs.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + gnuConstraint(P.InputConstraints[I]) + "\"((" +
            unsignedType(P.InputBytes[I]) + ")(" + Inputs[I] + "))";
  }
  Text += " : ";
  if (P.ChangesFlags)
    Text += "\"cc\", ";
  return Text + "\"memory\");";
}

bool matchesReturn(const llvm::CallInst &Call, const X86SystemStateAsm &P) {
  const auto *T = Call.getType();
  if (P.OutputBytes.empty())
    return T->isVoidTy();
  if (P.OutputBytes.size() == 1)
    return T->isIntegerTy(8 * P.OutputBytes[0]);
  const auto *S = llvm::dyn_cast<llvm::StructType>(T);
  return S && S->isLiteral() && !S->isPacked() && S->getNumElements() == 2 &&
         S->getElementType(0)->isIntegerTy(8 * P.OutputBytes[0]) &&
         S->getElementType(1)->isIntegerTy(8);
}

std::string locals(const X86SystemStateAsm &P,
                   std::vector<std::string> &Names) {
  std::string Text;
  for (size_t I = 0; I < P.OutputBytes.size(); ++I) {
    Names.push_back("__neverd_system_" + std::to_string(I));
    Text += unsignedType(P.OutputBytes[I]) + " " + Names.back() + "; ";
  }
  return Text;
}

} // namespace

std::string
renderX86SystemStateExpression(Arch TargetArch, const HighExpr &Expression,
                               const std::vector<std::string> &Arguments) {
  const auto Id = Expression.IntrinsicId;
  const auto C = x86SystemStateContract(Id);
  X86SystemStateShape Shape;
  Shape.TargetArch = TargetArch;
  Shape.ValidId = Expression.Kind == ExprKind::Call &&
                  Expression.IntrinsicOutputs.empty() &&
                  Expression.Operands.size() == Arguments.size();
  Shape.WritableOutput = true;
  Shape.OutputSize = Expression.Type ? Expression.Type->Size : 0;
  Shape.Arguments = Expression.Operands.size();
  Shape.MemoryOrdering = Expression.MemoryOrdering;
  Shape.MemoryAddressSpace = Expression.MemoryAddressSpace;
  for (size_t I = 0; I < Shape.Arguments && I < Shape.InputSizes.size(); ++I) {
    const auto &E = Expression.Operands[I];
    Shape.ScalarInputs[I] =
        E && E->Type &&
        (E->Type->Kind == NdTypeKind::Int || E->Type->Kind == NdTypeKind::Ptr);
    Shape.InputSizes[I] = E && E->Type ? E->Type->Size : 0;
    if (isX86SystemStateAddressArgument(Id, I) && E && E->Type &&
        E->Type->Kind == NdTypeKind::Ptr)
      Shape.InputSizes[I] = 8;
  }
  if (!x86SystemStateShapeIsValid(Id, Shape))
    throw std::invalid_argument("invalid x86 system expression contract");
  const auto P = x86SystemStateAsm(
      Id, TargetArch, x86SystemStateWidth(Id, Shape), Shape.MemoryAddressSpace);
  if (!P)
    throw std::invalid_argument("unsupported x86 system assembly contract");
  std::vector<std::string> Outputs;
  std::string Text = "({ " + locals(*P, Outputs);
  Text += renderAsm(*P, Outputs, Arguments);
  if (Outputs.size() == 1)
    Text += " " + Outputs[0] + ";";
  else if (Outputs.size() == 2) {
    std::string Value = Outputs[0];
    if (C.Kind == X86SystemStateKind::AccessRights)
      Value = "(" + Outputs[1] + " ? " + Value + " : 0)";
    const auto Wide = unsignedType(Shape.OutputSize);
    Text += " (" + Wide + ")" + Value + " | ((" + Wide + ")" + Outputs[1] +
            " << " + std::to_string(8 * P->OutputBytes[0]) + ");";
  }
  return Text + " })";
}

std::optional<std::string>
renderX86SystemStateCall(Arch TargetArch, const llvm::CallInst &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || (TargetArch != Arch::X86 && TargetArch != Arch::X64))
    return {};
  const Intrinsic Inventory[] = {
#define NEVERD_X86_SYSTEM_STATE(Id, ...) Intrinsic::Id,
#define NEVERD_X86_EXISTING_SYSTEM_STATE NEVERD_X86_SYSTEM_STATE
#include "neverd/ir/intrinsics/X86SystemState.def"
#undef NEVERD_X86_EXISTING_SYSTEM_STATE
#undef NEVERD_X86_SYSTEM_STATE
  };
  bool Known = false;
  auto Mnemonic = llvm::StringRef(Asm->getAsmString()).split('\n').first;
  if (Mnemonic.starts_with("fs ") || Mnemonic.starts_with("gs "))
    Mnemonic = Mnemonic.drop_front(3);
  Mnemonic = Mnemonic.split(' ').first;
  for (const auto Id : Inventory) {
    const auto C = x86SystemStateContract(Id);
    if (Mnemonic != C.Mnemonic &&
        !(C.Kind == X86SystemStateKind::ShadowWrite &&
          (Mnemonic == std::string(C.Mnemonic) + "d" ||
           Mnemonic == std::string(C.Mnemonic) + "q")))
      continue;
    Known = true;
    for (uint16_t Width : {0, 2, 4, 8})
      for (const auto Space :
           {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
            NdMemoryAddressSpace::X86GS}) {
        const auto P = x86SystemStateAsm(Id, TargetArch, Width, Space);
        if (!P || P->Text != Asm->getAsmString() ||
            P->Constraints != Asm->getConstraintString() ||
            !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
            Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
            Call.getNumOperandBundles() || Call.onlyReadsMemory() ||
            Call.onlyWritesMemory() ||
            Call.arg_size() != P->InputBytes.size() ||
            Arguments.size() != Call.arg_size() || !matchesReturn(Call, *P))
          continue;
        bool Types = true;
        for (size_t I = 0; I < P->InputBytes.size(); ++I) {
          const auto *T = Call.getArgOperand(I)->getType();
          if (isX86SystemStateAddressArgument(Id, I) &&
              (I != C.MemoryArgument || Space == NdMemoryAddressSpace::Default))
            Types &= T->isPointerTy() && T->getPointerAddressSpace() == 0;
          else
            Types &= T->isIntegerTy(8 * P->InputBytes[I]);
        }
        if (!Types)
          continue;
        if (P->OutputBytes.empty())
          return renderAsm(*P, {}, Arguments) + '\n';
        if (ResultLive) {
          if (Result.empty())
            throw std::invalid_argument("system result has no C identity");
          return renderAsm(*P,
                           P->OutputBytes.size() == 1
                               ? std::vector<std::string>{Result}
                               : std::vector<std::string>{Result + ".field_0",
                                                          Result + ".field_1"},
                           Arguments) +
                 '\n';
        }
        std::vector<std::string> Outputs;
        std::string Text = "{ " + locals(*P, Outputs);
        return Text + renderAsm(*P, Outputs, Arguments) + " }\n";
      }
  }
  if (Known)
    throw std::invalid_argument(
        "unsupported x86 system inline assembly contract");
  return {};
}

} // namespace neverd
