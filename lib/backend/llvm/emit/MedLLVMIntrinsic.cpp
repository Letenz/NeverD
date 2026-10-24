//===- MedLLVMIntrinsic.cpp - INTRINSIC dispatch & inline asm ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// INTRINSIC opcode dispatch and inline-asm helper.  Architecture-specific
/// side-effect emitters live in X86/MedLLVMX86Sideeffect.cpp,
/// AArch64/MedLLVMAArch64Sideeffect.cpp, and ARM/MedLLVMARMSideeffect.cpp.
/// Value-producing emitters in X86/MedLLVMX86ValueEmitter.cpp,
/// AArch64/MedLLVMAArch64ValueEmitter.cpp, and ARM/MedLLVMARMValueEmitter.cpp.
/// x86 SIMD intrinsics live in X86/MedLLVMX86SimdEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/LLVMSyscallAsm.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/ir/X87BCD.h"
#include "neverd/ir/X87Constant.h"
#include "neverd/ir/intrinsics/X86Sgx.h"

#define DEBUG_TYPE "neverd-med-llvm-intrinsic"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X64Syscall.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedRawEnvironment.h"
#include "neverd/ir/med/MedSyscallConvention.h"
#include "neverd/ir/med/X86RegistrationSEHRegisters.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

namespace neverd {

std::optional<uint8_t>
MedLLVMEmitter::atomicIntrinsicAddressInput(const MedOp &Op) const {
  if (TargetArch != Arch::AArch64 || Op.NumInputs == 0 ||
      !Op.Inputs[0].isConst())
    return std::nullopt;

  using I = Intrinsic;
  const auto IC = static_cast<I>(Op.Inputs[0].ConstVal);
  switch (IC) {
  case I::A64_AtomicAnd:
  case I::A64_AtomicOr:
  case I::A64_AtomicXor:
  case I::A64_AtomicSmax:
  case I::A64_AtomicSmin:
  case I::A64_AtomicUmax:
  case I::A64_AtomicUmin:
    return Op.NumInputs >= 3 ? std::optional<uint8_t>(2) : std::nullopt;
  case I::A64_Ldxr:
  case I::A64_Ldaxr:
  case I::A64_Ldxp:
  case I::A64_Ldaxp:
    return Op.NumInputs >= 2 ? std::optional<uint8_t>(1) : std::nullopt;
  case I::A64_Stxr:
  case I::A64_Stlxr:
  case I::A64_Stxp:
  case I::A64_Stlxp:
    return Op.NumInputs >= 3 ? std::optional<uint8_t>(2) : std::nullopt;
  case I::A64_Rcwcasp:
  case I::A64_Rcwcaspa:
  case I::A64_Rcwcaspal:
  case I::A64_Rcwcaspl:
  case I::A64_Rcwscasp:
  case I::A64_Rcwscaspa:
  case I::A64_Rcwscaspal:
  case I::A64_Rcwscaspl:
    return Op.NumInputs >= 4 ? std::optional<uint8_t>(3) : std::nullopt;
  case I::A64_Ldclrp:
  case I::A64_Ldclrpa:
  case I::A64_Ldclrpal:
  case I::A64_Ldclrpl:
  case I::A64_Ldsetp:
  case I::A64_Ldsetpa:
  case I::A64_Ldsetpal:
  case I::A64_Ldsetpl:
    return Op.NumInputs >= 3 ? std::optional<uint8_t>(2) : std::nullopt;
  default:
    return std::nullopt;
  }
}

//===----------------------------------------------------------------------===//
// INTRINSIC dispatch
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitIntrinsic(const MedOp &Op,
                                           llvm::IRBuilder<> &Builder) {
  if (isMedRawEnvironmentRequirement(Op))
    llvm::report_fatal_error(
        "raw environment requirement has no LLVM runtime contract");
  if (!hasValidMedIntrinsicId(Op))
    llvm::report_fatal_error("intrinsic has an invalid intrinsic ID shape");
  const auto IntrCode = static_cast<uint16_t>(Op.Inputs[0].ConstVal);

  using I = Intrinsic;
  auto IC = static_cast<I>(IntrCode);
  if (isPortableX87Intrinsic(IC))
    return emitPortableX87Intrinsic(Op, Builder);

  if (IC == Intrinsic::X86AMXState)
    return emitX86AmxState(Op, Builder);
  if (isX86SgxIntrinsic(IC))
    return emitX86Sgx(Op, IC, Builder);
  if (isX86SystemStateIntrinsic(IC) || IC == I::X86MsrAccess)
    return emitX86SystemState(Op, IC, Builder);

  if (isX86VirtualizationIntrinsic(IC))
    return emitX86Virtualization(Op, IC, Builder);

  if (x86_registration::isSEHRegisterIntrinsic(Op))
    return emitSEHRegisterIntrinsic(Op, Builder);

  if (isX87ContextIntrinsic(IC)) {
    if (TargetArch != Arch::X64)
      llvm::report_fatal_error("complete x87 context currently requires x64");
    return emitX86IntrinsicValue(Op, IC, Builder);
  }

  if (IC == I::CetRdSsp || isX86FPStateIntrinsic(IC) ||
      isX87ConstantControlIntrinsic(IC) || isX87GuardedValueIntrinsic(IC) ||
      x87ValueInfo(IC)) {
    if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
      llvm::report_fatal_error("x86 FP state requires an x86 target");
    return emitX86IntrinsicValue(Op, IC, Builder);
  }

  if (IC == I::X86RequireDivPrecondition && TargetArch != Arch::X86 &&
      TargetArch != Arch::X64)
    llvm::report_fatal_error("x86 divide precondition requires an x86 target");

  // LowIR carries AMX as exact 64-byte TILECFG and 1-KiB tile values.  A
  // faithful compiled lowering additionally needs checked strided bulk memory,
  // restart progress in TILECFG.start_row, and an exception continuation that
  // publishes partially completed architectural state.  The sealed runtime
  // ABI currently exposes only scalar memory helpers and no tile-state slots;
  // declaring an ambient helper here would bypass that ABI and turn faults or
  // cross-function tile state into guesses.  Reject the complete family before
  // any target handler can emit an observable instruction.  Once a versioned
  // runtime contract owns those facilities, this single guard is the handoff
  // point for an exact AMX value/side-effect emitter.
  if (isAMXIntrinsic(IC))
    llvm::report_fatal_error(
        "AMX LLVM lowering requires a versioned tile-state, "
        "restartable-memory, and fault-continuation runtime ABI");

  if (isPdepPextIntrinsic(IC) &&
      !intrinsicPdepPextShapeIsValid(IC, pdepPextMedShape(Op)))
    llvm::report_fatal_error(
        "PDEP/PEXT intrinsic has an invalid operand/output contract");

  // LLVM has no stable target-independent representation for the contiguous
  // packed-address fault contract used by EVEX compress/expand in the LLVM
  // version supported by this tree.  Never turn a memory effect into zero.
  if (IC == I::EVEXCompressStore || IC == I::EVEXExpandLoad)
    llvm::report_fatal_error(
        "EVEX compress/expand memory lowering is not available");

  if (IC == I::Vdbpsadbw) {
    if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
      llvm::report_fatal_error("VDBPSADBW intrinsic requires an x86 target");
    return emitVdbpsadbwIntrinsic(Op, IC, Builder);
  }
  if (IC == I::X86FourFMA)
    llvm::report_fatal_error(
        "x86 four-iteration FMA lowering is not available");
  if (isX86VP4DPIntrinsic(IC))
    llvm::report_fatal_error(
        "x86 VP4DP word dot-product lowering is not available");
  if (IC == I::ApxRaoAdd || IC == I::ApxRaoAnd || IC == I::ApxRaoOr ||
      IC == I::ApxRaoXor || IC == I::ApxCmpccXadd)
    llvm::report_fatal_error("APX atomic LLVM lowering is not available");

  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
    if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
      llvm::report_fatal_error(
          "FS/GS memory address spaces require an x86 target");
    if (!intrinsicSupportsMemoryAddressSpace(IC))
      llvm::report_fatal_error(
          "intrinsic does not support a memory address space");
    if (!intrinsicMemoryAddressSpaceShapeIsValid(
            IC, Op.NumInputs, Op.Output.Size,
            Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
            Op.NumInputs > 2 ? Op.Inputs[2].Size : 0,
            Op.NumInputs > 3 ? Op.Inputs[3].Size : 0))
      llvm::report_fatal_error(
          "segmented-memory intrinsic has an invalid operand/output shape");
  }

  if (emitSideeffectIntrinsic(Op, IC, Builder))
    return nullptr;

  // Masked stores are value-dispatch intrinsics with a void result.  They must
  // be dispatched before the generic zero-output early return; otherwise the
  // observable store is silently dropped.  The arity check also makes the
  // handler's nullptr-success convention unambiguous.
  if ((TargetArch == Arch::X86 || TargetArch == Arch::X64) &&
      (IC == I::MaskedStoreB || IC == I::MaskedStoreW ||
       IC == I::MaskedStoreD || IC == I::MaskedStoreQ)) {
    if (Op.NumInputs < 4 || Op.Output.Size != 0)
      llvm::report_fatal_error(
          "masked-store intrinsic has an invalid operand/output shape");
    (void)emitMaskedMemOp(Op, IC, Builder);
    return nullptr;
  }

  // GFNI is an x86 value-producing family.  Dispatch it before the generic
  // void early return so a malformed zero-width shape cannot be mistaken for
  // a successfully handled side effect or a non-x86 zero fallback.
  const bool IsGfni =
      IC == I::Gf2p8MulB || IC == I::Gf2p8AffineQb || IC == I::Gf2p8AffineInvQb;
  if (IsGfni) {
    if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
      llvm::report_fatal_error("GFNI intrinsic requires an x86 target");
    return emitGfniIntrinsic(Op, IC, Builder);
  }

  if (Op.Output.Size == 0)
    return nullptr;

  auto *OutTy = sizeToType(Op.Output.Size);

  if (const SyscallConvention *Syscall = modeledSyscallConvention(IC))
    return emitLinuxSyscall(Op, *Syscall, Builder);

  llvm::Value *R = nullptr;

  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    if ((R = emitAesIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitShaIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitShuffleIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitPackedShift(Op, IC, Builder)))
      return R;
    if ((R = emitMiscSimd(Op, IC, Builder)))
      return R;
    if ((R = emitX86IntrinsicValue(Op, IC, Builder)))
      return R;
    break;
  case Arch::AArch64:
    if ((R = emitAArch64IntrinsicValue(Op, IC, Builder)))
      return R;
    break;
  case Arch::ARM:
    if ((R = emitARMIntrinsicValue(Op, IC, Builder)))
      return R;
    break;
  default:
    break;
  }

  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error(
        "segmented-memory intrinsic was not handled by the target backend");

  if (IC == I::Hlt) {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::debugtrap);
    Builder.CreateCall(Fn, {});
    return (Op.Output.Size > 0) ? llvm::ConstantInt::get(OutTy, 0) : nullptr;
  }

  {
    // The bare-mnemonic inline-asm fallback is for VOID side-effect intrinsics
    // only (dsb/isb/wfe/...).  A value-producing intrinsic (Output.Size > 0)
    // reaching here has no real handler: emitting its mnemonic with bare `r`
    // operands yields malformed asm (e.g. a 1-operand `sel`/`ssat`) that aborts
    // codegen and crashes the whole process.  Fall through to the warning +
    // safe 0 so it surfaces as a test mismatch instead (see #270d ArmSsat, #280
    // ArmSel).
    const char *AsmStr = intrinsicAsmMnemonic(IC);
    if (AsmStr && Op.Output.Size == 0) {
      emitVoidInlineAsm(AsmStr, Op, Builder);
      return nullptr;
    }
  }

  syncWarning() << "INTRINSIC unhandled intrinsic: code=" << IntrCode << " ("
                << intrinsicName(IC) << ") out_sz=" << Op.Output.Size
                << " n_in=" << static_cast<unsigned>(Op.NumInputs) << "\n";
  if (Op.Output.Size > 0)
    ++UnhandledValueIntrinsicCount;
  return (Op.Output.Size > 0) ? llvm::ConstantInt::get(OutTy, 0) : nullptr;
}

//===----------------------------------------------------------------------===//
// Inline asm helper
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::emitVoidInlineAsm(const char *Mnemonic, const MedOp &Op,
                                       llvm::IRBuilder<> &Builder) {
  auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
  std::vector<llvm::Type *> ParamTys;
  std::vector<llvm::Value *> ParamVals;
  for (uint16_t I = 1; I < Op.NumInputs; ++I) {
    auto *V = getVar(Op.Inputs[I], Builder);
    ParamTys.push_back(V->getType());
    ParamVals.push_back(V);
  }
  std::string Cstr;
  for (size_t I = 0; I < ParamVals.size(); ++I)
    Cstr += (Cstr.empty() ? "r" : ",r");
  Cstr += (Cstr.empty() ? "~{memory}" : ",~{memory}");
  auto *AsmFnTy = llvm::FunctionType::get(VoidTy, ParamTys, false);
  auto *IA = llvm::InlineAsm::get(AsmFnTy, Mnemonic, Cstr, true);
  Builder.CreateCall(IA, ParamVals);
}

//===----------------------------------------------------------------------===//
// Side-effect intrinsic dispatcher
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitSideeffectIntrinsic(const MedOp &Op, Intrinsic IC,
                                             llvm::IRBuilder<> &Builder) {
  // An SMC or HVC #0 runs under the SMC Calling Convention with its registers
  // pinned: the function identifier and arguments in x0-x7 (r0-r7), the
  // results out of x0-x3, which bind the outputs the lift gave it.
  if (Op.NumInputs >= 2 && Op.Inputs[1].isConst())
    if (const llvm::ArrayRef<uint64_t> Arguments =
            smcccArgumentRegisters(TargetArch, IC, Op.Inputs[1].ConstVal);
        !Arguments.empty() && Op.NumInputs == 2 + Arguments.size()) {
      const bool AArch64 = TargetArch == Arch::AArch64;
      auto *RegisterTy = AArch64 ? Builder.getInt64Ty() : Builder.getInt32Ty();
      const std::string Prefix = AArch64 ? "x" : "r";
      std::vector<llvm::Value *> Values;
      for (unsigned Index = 0; Index < Arguments.size(); ++Index) {
        llvm::Value *Value = getVar(Op.Inputs[2 + Index], Builder);
        Values.push_back(Value->getType()->isPointerTy()
                             ? Builder.CreatePtrToInt(Value, RegisterTy)
                             : Builder.CreateZExtOrTrunc(Value, RegisterTy));
      }
      std::string Constraints;
      for (unsigned Index = 0; Index < 4; ++Index)
        Constraints += "={" + Prefix + std::to_string(Index) + "},";
      Constraints += "0,1,2,3";
      for (unsigned Index = 4; Index < Arguments.size(); ++Index)
        Constraints += ",{" + Prefix + std::to_string(Index) + "}";
      Constraints += ",~{memory}";
      auto *ResultTy = llvm::StructType::get(
          *Ctx, {RegisterTy, RegisterTy, RegisterTy, RegisterTy});
      std::vector<llvm::Type *> Types(Values.size(), RegisterTy);
      auto *Call = Builder.CreateCall(
          llvm::InlineAsm::get(llvm::FunctionType::get(ResultTy, Types, false),
                               intrinsicInstructionText(IC, 0), Constraints,
                               /*hasSideEffects=*/true),
          Values, "smccc");
      for (unsigned Index = 0; Index < 4; ++Index)
        PendingIntrinsicOutputs[Index] =
            Builder.CreateExtractValue(Call, {Index}, "smccc.result");
      PendingIntrinsicCount = 4;
      if (Op.Output.Size > 0)
        setVar(Op.Output,
               Builder.CreateZExtOrTrunc(PendingIntrinsicOutputs[0],
                                         sizeToType(Op.Output.Size)),
               Builder);
      return true;
    }
  bool Matched = false;
  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    Matched = emitX86Sideeffect(Op, IC, Builder);
    break;
  case Arch::AArch64:
    Matched = emitAArch64Sideeffect(Op, IC, Builder);
    break;
  case Arch::ARM:
    Matched = emitARMSideeffect(Op, IC, Builder);
    break;
  default:
    break;
  }
  if (Matched && Op.Output.Size > 0) {
    auto *OutTy = sizeToType(Op.Output.Size);
    setVar(Op.Output, llvm::ConstantInt::get(OutTy, 0), Builder);
  }
  return Matched;
}

llvm::Value *
MedLLVMEmitter::emitLinuxSyscall(const MedOp &Op,
                                 const SyscallConvention &Convention,
                                 llvm::IRBuilder<> &Builder) {
  if (TargetArch != Convention.TheArch || TargetFormat != BinaryFormat::ELF ||
      !llvm::Triple(Mod->getTargetTriple()).isOSLinux())
    llvm::report_fatal_error(
        "a Linux supervisor call requires the Linux execution ABI");
  if (Op.NumInputs != 2u + Convention.ArgumentCount ||
      Op.Output.Size != Convention.RegisterSize ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    llvm::report_fatal_error("invalid Linux supervisor call register operands");
  auto *RegisterTy = sizeToType(Convention.RegisterSize);
  // getVar can materialize a known scalar through register backing loads.
  // Use the relocation-aware MedIR control proof to identify the syscall.
  const auto KnownNumber = controlConstantMayRelocate(Op.Inputs[1])
                               ? std::nullopt
                               : traceControlConst(Op.Inputs[1]);
  const std::optional<unsigned> Read =
      KnownNumber ? linuxSyscallArgumentCount(Convention.TheArch, *KnownNumber)
                  : std::nullopt;
  std::vector<llvm::Value *> Arguments = {
      Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), RegisterTy)};
  for (unsigned I = 0; I < Convention.ArgumentCount; ++I)
    // A verified syscall reads only its own arguments: invent no dependency
    // on an undefined register it ignores.
    Arguments.push_back(
        Read && I >= *Read
            ? llvm::ConstantInt::get(RegisterTy, 0)
            : Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2 + I], Builder),
                                        RegisterTy));
  std::vector<llvm::Type *> Types(Arguments.size(), RegisterTy);
  auto *Asm = llvm::InlineAsm::get(
      llvm::FunctionType::get(RegisterTy, Types, false),
      syscallAsmString(Convention), syscallAsmConstraints(Convention), true);
  return Builder.CreateCall(Asm, Arguments, "syscall_result");
}

} // namespace neverd
