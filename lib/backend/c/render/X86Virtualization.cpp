//===- X86Virtualization.cpp - VMX/SVM source rendering ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Render the owned architectural assembly contract without speculating about
/// VMCS status or materializing conditional memory operands as C accesses.
//===----------------------------------------------------------------------===//

#include "X86Virtualization.h"

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

std::string gnuConstraint(const std::string &Constraint) {
  if (Constraint == "={ax}")
    return "=a";
  if (Constraint == "{ax}")
    return "a";
  if (Constraint == "{cx}")
    return "c";
  return Constraint;
}

std::string gnuText(const std::string &Text) {
  std::string Out;
  for (const char C : Text) {
    switch (C) {
    case '$':
      Out += '%';
      break;
    case '%':
      Out += "%%";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\t':
      Out += "\\t";
      break;
    default:
      Out += C;
      break;
    }
  }
  return Out;
}

std::string renderAsm(const X86VirtualizationAsm &Plan,
                      const std::vector<std::string> &Outputs,
                      const std::vector<std::string> &Inputs) {
  if (Outputs.size() != Plan.OutputConstraints.size() ||
      Inputs.size() != Plan.InputConstraints.size())
    throw std::invalid_argument("invalid x86 virtualization assembly operands");
  std::string Text = "__asm__ volatile(\"" + gnuText(Plan.Text) + "\" : ";
  for (size_t I = 0; I < Outputs.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + gnuConstraint(Plan.OutputConstraints[I]) + "\"(" +
            Outputs[I] + ")";
  }
  Text += " : ";
  for (size_t I = 0; I < Inputs.size(); ++I) {
    if (I)
      Text += ", ";
    Text += "\"" + gnuConstraint(Plan.InputConstraints[I]) + "\"((" +
            unsignedType(Plan.InputBytes[I]) + ")(" + Inputs[I] + "))";
  }
  Text += " : ";
  if (!Outputs.empty())
    Text += "\"cc\", ";
  return Text + "\"memory\");";
}

bool resultMatches(const llvm::CallInst &Call, Intrinsic Id, uint16_t Word) {
  switch (x86VirtualizationContract(Id).Result) {
  case X86VirtualizationResult::None:
    return Call.getType()->isVoidTy();
  case X86VirtualizationResult::Flags:
    return Call.getType()->isIntegerTy(16);
  case X86VirtualizationResult::Read: {
    const auto *T = llvm::dyn_cast<llvm::StructType>(Call.getType());
    return T && T->isLiteral() && !T->isPacked() && T->getNumElements() == 2 &&
           T->getElementType(0)->isIntegerTy(8 * Word) &&
           T->getElementType(1)->isIntegerTy(16);
  }
  }
  return false;
}

} // namespace

std::string
renderX86VirtualizationExpression(Arch TargetArch, const HighExpr &Expression,
                                  const std::vector<std::string> &Arguments) {
  const auto Id = Expression.IntrinsicId;
  const auto C = x86VirtualizationContract(Id);
  if (!C.Mnemonic)
    return {};
  X86VirtualizationShape Shape;
  Shape.TargetArch = TargetArch;
  Shape.ValidId = Expression.Kind == ExprKind::Call &&
                  Expression.IntrinsicOutputs.empty() &&
                  Expression.Operands.size() == C.Arguments;
  Shape.WritableOutput = true;
  Shape.OutputSize = Expression.Type ? Expression.Type->Size : 0;
  Shape.Arguments = Expression.Operands.size();
  Shape.MemoryOrdering = Expression.MemoryOrdering;
  Shape.MemoryAddressSpace = Expression.MemoryAddressSpace;
  for (size_t I = 0;
       I < Expression.Operands.size() && I < Shape.InputSizes.size(); ++I) {
    const auto &E = Expression.Operands[I];
    Shape.ScalarInputs[I] =
        E && E->Type &&
        (E->Type->Kind == NdTypeKind::Int || E->Type->Kind == NdTypeKind::Ptr);
    Shape.InputSizes[I] = E && E->Type ? E->Type->Size : 0;
    if (I == C.MemoryArgument && E && E->Type &&
        E->Type->Kind == NdTypeKind::Ptr)
      Shape.InputSizes[I] = 8;
  }
  if (!x86VirtualizationShapeIsValid(Id, Shape) ||
      Arguments.size() != C.Arguments)
    throw std::invalid_argument(
        "x86 virtualization expression has an invalid operand/output contract");
  const auto Plan =
      x86VirtualizationAsm(Id, TargetArch, Shape.MemoryAddressSpace,
                           C.PhysicalAddress ? Shape.InputSizes[0] : 0);
  if (!Plan)
    throw std::invalid_argument(
        "unsupported x86 virtualization assembly contract");
  if (C.Result == X86VirtualizationResult::None)
    return "({ " + renderAsm(*Plan, {}, Arguments) + " })";
  const bool Read = C.Result == X86VirtualizationResult::Read;
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  std::string Text = "({ uint16_t __neverd_vmx_flags; ";
  if (Read)
    Text += unsignedType(Word) + " __neverd_vmx_value; ";
  Text += renderAsm(*Plan,
                    Read ? std::vector<std::string>{"__neverd_vmx_value",
                                                    "__neverd_vmx_flags"}
                         : std::vector<std::string>{"__neverd_vmx_flags"},
                    Arguments);
  if (Read) {
    const auto Wide = unsignedType(2 * Word);
    Text += " (" + Wide +
            ")(__neverd_vmx_flags ? 0 : __neverd_vmx_value) | ((" + Wide +
            ")__neverd_vmx_flags << " + std::to_string(8 * Word) + ")";
  } else
    Text += " __neverd_vmx_flags";
  return Text + "; })";
}

std::optional<std::string>
renderX86VirtualizationCall(Arch TargetArch, const llvm::CallInst &Call,
                            const std::string &Result, bool ResultLive,
                            const std::vector<std::string> &Arguments) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || (TargetArch != Arch::X86 && TargetArch != Arch::X64))
    return {};
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  bool Known = false;
  const auto Mnemonic =
      llvm::StringRef(Asm->getAsmString()).split('\n').first.split(' ').first;
  const Intrinsic Inventory[] = {
#define NEVERD_X86_VIRTUALIZATION(Id, ...) Intrinsic::Id,
#include "neverd/ir/intrinsics/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
  };
  for (const auto Id : Inventory) {
    const auto C = x86VirtualizationContract(Id);
    if (Mnemonic != C.Mnemonic)
      continue;
    Known = true;
    for (const auto Space :
         {NdMemoryAddressSpace::Default, NdMemoryAddressSpace::X86FS,
          NdMemoryAddressSpace::X86GS}) {
      const uint16_t PhysicalBytes =
          C.PhysicalAddress && Call.arg_size() &&
                  Call.getArgOperand(0)->getType()->isIntegerTy()
              ? Call.getArgOperand(0)->getType()->getIntegerBitWidth() / 8
              : 0;
      const auto P = x86VirtualizationAsm(Id, TargetArch, Space, PhysicalBytes);
      if (!P || P->Text != Asm->getAsmString() ||
          P->Constraints != Asm->getConstraintString() ||
          !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
          Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
          Call.getNumOperandBundles() || Call.doesNotAccessMemory() ||
          Call.arg_size() != P->InputBytes.size() ||
          Arguments.size() != Call.arg_size() || !resultMatches(Call, Id, Word))
        continue;
      bool Types = true;
      for (size_t I = 0; I < P->InputBytes.size(); ++I) {
        const auto *T = Call.getArgOperand(I)->getType();
        if (I == C.MemoryArgument && Space == NdMemoryAddressSpace::Default)
          Types &= T->isPointerTy() && T->getPointerAddressSpace() == 0;
        else
          Types &= T->isIntegerTy(8 * P->InputBytes[I]);
      }
      if (!Types)
        continue;
      if (C.Result == X86VirtualizationResult::None)
        return renderAsm(*P, {}, Arguments) + '\n';
      const bool Read = C.Result == X86VirtualizationResult::Read;
      if (ResultLive && Result.empty())
        throw std::invalid_argument(
            "x86 virtualization result has no source identity");
      if (ResultLive)
        return renderAsm(*P,
                         Read ? std::vector<std::string>{Result + ".field_0",
                                                         Result + ".field_1"}
                              : std::vector<std::string>{Result},
                         Arguments) +
               '\n';
      std::string Text = "{ uint16_t __neverd_vmx_flags; ";
      if (Read)
        Text += unsignedType(Word) + " __neverd_vmx_value; ";
      return Text +
             renderAsm(*P,
                       Read ? std::vector<std::string>{"__neverd_vmx_value",
                                                       "__neverd_vmx_flags"}
                            : std::vector<std::string>{"__neverd_vmx_flags"},
                       Arguments) +
             " }\n";
    }
  }
  if (Known)
    throw std::invalid_argument(
        "unsupported x86 virtualization inline assembly contract");
  return {};
}

} // namespace neverd
