#include "ImmutableNativeFrame.h"

#include "../SourceUnwind.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/loader/MachO/DarwinImportVeneer.h"
#include "neverd/loader/MachO/RuntimeFunctionAddress.h"
#include "neverd/loader/ObjC/ObjCCallHints.h"
#include "neverd/loader/ObjC/ObjCContextCallEffects.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/Swift/SwiftAccessEffects.h"
#include "neverd/loader/Swift/SwiftRuntimeCalls.h"
#include "neverd/loader/Swift/SwiftValueBufferEffects.h"

namespace neverd {
namespace {
bool sameOperation(const LowOp &A, const LowOp &B) {
  if (A.Opcode != B.Opcode || A.Addr != B.Addr || A.Seq != B.Seq ||
      A.NumInputs != B.NumInputs || A.NumInputs > 6 ||
      A.MemoryOrdering != B.MemoryOrdering ||
      A.MemoryAddressSpace != B.MemoryAddressSpace || A.Output != B.Output)
    return false;
  for (unsigned I = 0; I < A.NumInputs; ++I)
    if (A.Inputs[I] != B.Inputs[I])
      return false;
  return true;
}
} // namespace

bool immutableNativeFrameMachineMatches(const BinaryImage &Image,
                                        const LowFunc &Function,
                                        size_t &Budget) {
  // Re-lift with the canonical decoder, including all frame address arithmetic,
  // stores, loads and flags. A saved LowIR annotation is not machine evidence.
  // This deliberately excludes rewritten tails, opaque exits and jump tables.
  if (Image.Format != BinaryFormat::MachO || Image.IsRelocatable ||
      Image.Arch != Arch::AArch64 || Image.Bits != Bitness::Bits64 ||
      !Function.Entry || Function.Entry % 4 ||
      !Function.DecodedInstructionCount ||
      Function.DecodedInstructionCount > 4096 ||
      !Function.hasCompleteLiftCoverage() || !Function.JumpTables.empty() ||
      (!Function.ModuleAnalysisRoots.empty() &&
       Function.ModuleAnalysisRoots != std::set<va_t>{Function.Entry}) ||
      (!Function.OrdinaryModuleAnalysisRoots.empty() &&
       Function.OrdinaryModuleAnalysisRoots !=
           std::set<va_t>{Function.Entry}) ||
      (Function.ExceptionMetadata &&
       !isPlainSourceUnwind(*Function.ExceptionMetadata)))
    return false;
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return false;
  }
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  std::map<int, va_t> Starts;
  std::set<va_t> Unique;
  std::set<va_t> Instructions;
  for (const auto &Block : Function.Blocks)
    if (!Starts.emplace(Block.Id, Block.StartAddr).second ||
        !Unique.insert(Block.StartAddr).second)
      return false;
  for (const auto &Block : Function.Blocks) {
    if (!Block.ExceptionalPreds.empty() || !Block.ExceptionalSuccs.empty())
      return false;
    va_t Next = Block.StartAddr;
    std::set<va_t> Successors;
    for (const auto &B : Block.InstructionBoundaries) {
      if (!Budget || B.OpCount > Budget - 1) {
        Budget = 0;
        return false;
      }
      if (B.FirstOp > Block.Ops.size() ||
          B.OpCount > Block.Ops.size() - B.FirstOp || B.Address != Next ||
          B.Size != 4 || B.Address % 4 || B.Address > UINT64_MAX - 4 ||
          !Instructions.insert(B.Address).second ||
          B.Mode != InstructionMode::Default ||
          B.TargetMode != LowInstructionTargetMode::Preserve)
        return false;
      Budget -= B.OpCount + 1;
      Next += 4;
      const auto Bytes = readImmutableCodeBytes(Image, B.Address, 4);
      DecodedInsn Insn;
      if (!Bytes ||
          Dec.decodeOneForLift(Bytes->data(), 4, B.Address, Insn) != 4)
        return false;
      std::vector<LowOp> Ops;
      try {
        Dec.liftToLow(Insn, Ops);
      } catch (const UnliftedInstruction &) {
        return false;
      }
      if (Ops.size() != B.OpCount)
        return false;
      LowInstructionControl Control = LowInstructionControl::None;
      LowInstructionControlFlag Flags = LowInstructionControlFlag::None;
      std::optional<uint64_t> Immediate;
      bool EndsBlock = false;
      Successors = {Next};
      for (size_t I = 0; I < Ops.size(); ++I) {
        const auto &Op = Ops[I];
        if (!sameOperation(Op, Block.Ops[B.FirstOp + I]))
          return false;
        using C = LowInstructionControl;
        using F = LowInstructionControlFlag;
        if (Op.Opcode == NdOp::INDIR_BR)
          return false;
        if (Op.Opcode != NdOp::BRANCH && Op.Opcode != NdOp::COND_BR &&
            Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL &&
            Op.Opcode != NdOp::RETURN)
          continue;
        if (Control != C::None)
          return false;
        if (Op.Opcode == NdOp::RETURN) {
          Control = C::Return;
          Flags = F::Return;
          EndsBlock = true;
          Successors.clear();
        } else if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          Control = C::Call;
          Flags = F::Call;
          if (Op.Opcode == NdOp::INDIR_CALL)
            Flags |= F::Indirect;
          else
            Immediate = Op.Inputs[0].Offset;
        } else {
          if (!Op.NumInputs || !Op.Inputs[0].isConst())
            return false;
          Control = C::Branch;
          Flags = F::Branch;
          Immediate = Op.Inputs[0].Offset;
          EndsBlock = true;
          if (Op.Opcode == NdOp::COND_BR)
            Flags |= F::Conditional;
          else
            Successors.clear();
          Successors.insert(*Immediate);
        }
      }
      if (Control != B.Control || Flags != B.ControlFlags ||
          Immediate != B.Immediate ||
          (EndsBlock && &B != &Block.InstructionBoundaries.back()))
        return false;
    }
    if (Block.InstructionBoundaries.empty() || Next != Block.EndAddr)
      return false;
    std::set<va_t> Actual;
    for (int Id : Block.Succs) {
      const auto Found = Starts.find(Id);
      if (Found == Starts.end() || !Actual.insert(Found->second).second)
        return false;
    }
    if (Successors != Actual)
      return false;
  }
  return Instructions.size() == Function.DecodedInstructionCount;
}

std::optional<SourceFunctionTypeHint> immutableNativeDirectCallABI(
    const BinaryImage &Image, va_t Target,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees) {
  std::optional<SourceFunctionTypeHint> Signature;
  if (const auto Slot = darwinImportVeneerSlot(Image, Target)) {
    if (const auto Runtime = runtimeCFunctionAddressHint(Image, *Slot))
      Signature = Runtime->AddressedFunctionABI;
    else if (isImmutableImageImportSlot(Image, *Slot)) {
      const auto ARC = objcRuntimeSourceCallHint(Image, *Slot);
      const auto Bind = Image.DyldBindSlots.find(*Slot);
      if (ARC && ARC->CallKind == SourceCallTypeHint::Kind::ObjCRuntimeCall &&
          ARC->TargetAddress == *Slot && !ARC->DoesNotReturn &&
          !ARC->WeakImport && Bind != Image.DyldBindSlots.end() &&
          Bind->second.Module == "/usr/lib/libobjc.A.dylib" &&
          ARC->Signature.Origin ==
              SourceFunctionTypeHint::OriginKind::ObjCRuntime)
        Signature = ARC->Signature;
    }
  } else if (const auto Message =
                 objcSelectorStubSourceCallHint(Image, Target)) {
    Signature = Message->Signature;
  } else if (NativeCallees) {
    const auto Found = NativeCallees->find(Target);
    if (Found != NativeCallees->end() &&
        Found->second.Origin ==
            SourceFunctionTypeHint::OriginKind::NativeAnalysis)
      Signature = Found->second;
  }
  std::string Error;
  if (!Signature || !Signature->HasExplicitABI ||
      Signature->Architecture != Image.Arch ||
      !validateSourceABI(*Signature, Error))
    return std::nullopt;
  return Signature;
}

ImmutableNativeFrameCalls immutableNativeFrameCalls(
    const BinaryImage &Image, const LowFunc &Function,
    const SourceLocalCalls &DirectCalls,
    const std::map<va_t, ImmutableNativeCallTarget> &KnownTargets,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees) {
  ImmutableNativeFrameCalls Result;
  const auto Add = [&](const NativeSourceCallKey &Site, va_t Target,
                       const SourceFunctionTypeHint &Signature) {
    SourceFrameEffects Effects;
    if (isSwiftAccessCallTarget(Image, Target)) {
      const auto Slot = darwinImportVeneerSlot(Image, Target);
      const auto Binding =
          Slot ? swiftRuntimeSourceCallHint(Image, *Slot) : std::nullopt;
      const auto Found =
          Binding && equalSourceABIs(Binding->Signature, Signature)
              ? swiftAccessCallEffects(Image, Function, Site, *Binding)
              : std::nullopt;
      if (!Found)
        return;
      Effects = *Found;
    } else if (isSwiftValueBufferProjection(Image, Target)) {
      const auto Found =
          swiftValueBufferProjectionEffects(Image, Target, Signature);
      if (!Found)
        return;
      Effects = *Found;
    } else if (isObjCContextProjection(Image, Target)) {
      const auto Found = objcContextProjectionEffects(Image, Target, Signature);
      if (!Found)
        return;
      Effects = *Found;
    }
    if (!sourceFrameEffectsMatchABI(Effects, Signature))
      return;
    const auto [It, Added] = Result.Signatures.emplace(Site, Signature);
    if (!Added)
      return;
    NativeSourceCallContract Contract;
    Contract.Signature = &It->second;
    static_cast<SourceFrameEffects &>(Contract) = std::move(Effects);
    Result.Calls.emplace(Site, std::move(Contract));
  };
  for (const auto &[Site, Word] : DirectCalls)
    if (const auto ABI = immutableNativeDirectCallABI(Image, *Site.StaticTarget,
                                                      NativeCallees))
      Add(Site, *Site.StaticTarget, *ABI);
  if (NativeCallees)
    for (const auto &[Address, Target] : KnownTargets) {
      const auto ABI = NativeCallees->find(Target.Target);
      if (ABI != NativeCallees->end() &&
          ABI->second.Origin ==
              SourceFunctionTypeHint::OriginKind::NativeAnalysis)
        Add(Target.Site, Target.Target, ABI->second);
    }
  return Result;
}
} // namespace neverd
