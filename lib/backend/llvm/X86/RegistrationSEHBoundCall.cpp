//===- RegistrationSEHBoundCall.cpp - Source preserved SEH calls ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Validate source identities and argument recipes for preserved SEH calls.

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationSEHBoundCall.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationCallResult.h"

#include "llvm/IR/DerivedTypes.h"

namespace neverd {

std::optional<std::map<va_t, X86RegistrationSEHBoundCall>>
getX86RegistrationSEHBoundCalls(const MedFunc &Function) {
  if (!Function.RegistrationStates)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  std::map<va_t, X86RegistrationSEHBoundCall> Result;
  if (!States.HasBoundSEHCallees && States.SEHBoundCalls.empty())
    return Result;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.SEHBoundCallsComplete ||
      !States.HasBoundSEHCallees || States.SEHBoundCalls.empty() ||
      !Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return std::nullopt;
  size_t Work = 0;
  for (const auto &Call : States.SEHBoundCalls) {
    if (++Work > limits::kMaxRegistrationEHStateWork || Call.OpSeq < 0 ||
        Call.Address > UINT32_MAX - 5 || Call.TargetVA > UINT32_MAX ||
        Call.Arguments.size() > 31 ||
        !Result.emplace(Call.Address, X86RegistrationSEHBoundCall{&Call})
             .second)
      return std::nullopt;
    for (size_t I = 0; I != Call.Arguments.size(); ++I) {
      const auto &Arg = Call.Arguments[I];
      using Kind = RegistrationSEHBoundArgument::Kind;
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      switch (Arg.ArgumentKind) {
      case Kind::Unused:
        if (Arg.Offset || Arg.Capture)
          return std::nullopt;
        break;
      case Kind::Scalar:
        if (Arg.Capture ||
            int64_t(Call.StackOffset) + int64_t(I * 4) != Arg.Offset)
          return std::nullopt;
        break;
      case Kind::Object:
        if (Arg.Capture || Arg.Offset > -28 ||
            Arg.Offset < int64_t(Call.StackOffset) + 128)
          return std::nullopt;
        break;
      case Kind::JumpBuffer:
        Work += States.SEHJumpCaptures.size();
        if (Work > limits::kMaxRegistrationEHStateWork || Arg.Offset ||
            !Arg.Capture ||
            !llvm::any_of(States.SEHJumpCaptures, [&](const auto &Capture) {
              return Capture.Address == Arg.Capture;
            }))
          return std::nullopt;
        break;
      default:
        return std::nullopt;
      }
    }
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Found = Result.find(Op.Addr);
      if (Found == Result.end() || Op.Opcode != NdOp::CALL)
        continue;
      auto &Call = Found->second;
      if (Call.Operation || Op.OriginSeq != Call.Source->OpSeq ||
          Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Size != 4 ||
          Op.Inputs[0].ConstVal != Call.Source->TargetVA ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return std::nullopt;
      Call.Operation = &Op;
      Call.Block = Block.Id;
    }
  if (llvm::any_of(Result,
                   [](const auto &Item) { return !Item.second.Operation; }))
    return std::nullopt;
  for (const auto &[Address, Call] : Result) {
    RegistrationCallFrameEffect Effect;
    Effect.Address = Address;
    Effect.OpSeq = Call.Source->OpSeq;
    Effect.Target = Call.Source->TargetVA;
    if (!hasUnusedX86RegistrationCallResult(Function, Effect, Work))
      return std::nullopt;
  }
  return Result;
}

llvm::FunctionType *
getX86RegistrationSEHBoundType(const RegistrationSEHBoundCall &Call,
                               llvm::LLVMContext &Context) {
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  llvm::SmallVector<llvm::Type *, 8> Arguments;
  for (const auto &Arg : Call.Arguments) {
    using Kind = RegistrationSEHBoundArgument::Kind;
    Arguments.push_back(Arg.ArgumentKind == Kind::Object ||
                                Arg.ArgumentKind == Kind::JumpBuffer
                            ? static_cast<llvm::Type *>(Ptr)
                            : I32);
  }
  return llvm::FunctionType::get(I32, Arguments, false);
}

} // namespace neverd
