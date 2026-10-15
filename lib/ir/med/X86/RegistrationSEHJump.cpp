//===- RegistrationSEHJump.cpp - Source nonlocal jump calls ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Bind checked nonlocal jump lifetimes to their surviving MedIR calls.

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationSEHBoundCall.h"
#include "neverd/ir/med/X86RegistrationSEHJump.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <set>

namespace neverd {

std::optional<std::map<va_t, X86RegistrationSEHJumpCall>>
getX86RegistrationSEHJumpCalls(const MedFunc &Function) {
  if (!Function.RegistrationStates || Function.SkippedSSA)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  const auto Bound = getX86RegistrationSEHBoundCalls(Function);
  if (!Bound)
    return std::nullopt;
  if (!States.SEHJumpCapturesComplete || !States.SEHJumpResumptionsComplete ||
      States.HasEH3JumpCaptures != !States.SEHJumpCaptures.empty())
    return std::nullopt;
  std::map<va_t, X86RegistrationSEHJumpCall> Result;
  if (States.SEHJumpCaptures.empty() && States.SEHJumpResumptions.empty())
    return Result;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.ExceptionMetadata->Personality !=
          ExceptionPersonality::ExceptHandler3)
    return std::nullopt;
  std::map<va_t, const MedBlock *> Blocks;
  std::map<int, const MedBlock *> BlocksById;
  std::map<int, const RegistrationBlockState *> BlockStates;
  std::map<va_t, const RegistrationSEHJumpCapture *> Captures;
  size_t Work = 0;
  for (const auto &State : States.Blocks)
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !BlockStates.emplace(State.BlockId, &State).second)
      return std::nullopt;
  for (const auto &Block : Function.Blocks) {
    if (++Work > limits::kMaxRegistrationEHStateWork ||
        !Blocks.emplace(Block.StartAddr, &Block).second ||
        !BlocksById.emplace(Block.Id, &Block).second)
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL &&
          !Result
               .emplace(Op.Addr, X86RegistrationSEHJumpCall{nullptr, nullptr,
                                                            &Op, Block.Id})
               .second)
        return std::nullopt;
    }
  }
  auto Bind = [&](va_t Address, int Seq) -> X86RegistrationSEHJumpCall * {
    const auto It = Result.find(Address);
    if (It == Result.end())
      return nullptr;
    const auto &Op = *It->second.Operation;
    if (Op.Dead || Op.ExactArguments || Op.SourceCallHint ||
        Op.OriginSeq != Seq || Seq < 0 || Op.NumInputs != 1 ||
        !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return nullptr;
    return &It->second;
  };
  for (const auto &Capture : States.SEHJumpCaptures) {
    auto *Call = Bind(Capture.Address, Capture.OpSeq);
    if (++Work > limits::kMaxRegistrationEHStateWork || !Call ||
        Call->Operation->Inputs[0].ConstVal != Capture.TargetVA ||
        Capture.Address > UINT32_MAX - 5 ||
        Capture.Address + 5 != Capture.EndAddress ||
        Capture.UnwindVA == InvalidVA || !Capture.UnwindVA ||
        int64_t(Capture.BufferOffset) + Capture.BufferBytes > -24 ||
        int64_t(Capture.StackOffset) + 16 > Capture.BufferOffset ||
        !Blocks.count(Capture.EndAddress) ||
        !Captures.emplace(Capture.Address, &Capture).second)
      return std::nullopt;
    const auto *Block = BlocksById.at(Call->Block);
    const auto State = BlockStates.find(Call->Block);
    if (Block->Succs.size() != 1 ||
        Block->Succs.front() != Blocks.at(Capture.EndAddress)->Id ||
        Block->EndAddr != Capture.EndAddress ||
        Call->Operation->DoesNotReturn || State == BlockStates.end() ||
        State->second->Unknown || State->second->CallbackOnly ||
        State->second->Levels.size() != 1 ||
        State->second->Levels.front() != Capture.Level)
      return std::nullopt;
    Call->Capture = &Capture;
  }
  struct Resumption {
    std::set<int32_t> Levels;
    bool StateInFrame = true;
  };
  std::map<va_t, Resumption> ResumeLevels;
  for (const auto &Resume : States.SEHJumpResumptions) {
    auto *Call = Bind(Resume.Address, Resume.OpSeq);
    const auto Capture = Captures.find(Resume.CaptureCall);
    if (++Work > limits::kMaxRegistrationEHStateWork || !Call ||
        Capture == Captures.end() || (Call->Capture && !Call->Resume) ||
        Resume.ResumedLevels.empty() ||
        Resume.TargetVA != Capture->second->EndAddress ||
        Resume.BufferOffset != Capture->second->BufferOffset ||
        Resume.StackOffset != Capture->second->StackOffset)
      return std::nullopt;
    if (Resume.CalleeVA || Resume.ResumeVA) {
      const auto Callee = Bound->find(Resume.Address);
      if (Callee == Bound->end() || Resume.ValueOffset || Resume.EntryVA ||
          Resume.OrdinaryCall || !Resume.ResumeVA ||
          Callee->second.Source->TargetVA != Resume.CalleeVA ||
          !llvm::any_of(Callee->second.Source->Arguments, [&](const auto &Arg) {
            return Arg.ArgumentKind ==
                       RegistrationSEHBoundArgument::Kind::JumpBuffer &&
                   Arg.Capture == Resume.CaptureCall;
          }))
        return std::nullopt;
    } else if (!Resume.ValueOffset || !Call->Operation->DoesNotReturn)
      return std::nullopt;
    Work += Resume.ResumedLevels.size();
    if (Work > limits::kMaxRegistrationEHStateWork ||
        !llvm::is_sorted(Resume.ResumedLevels) ||
        std::adjacent_find(Resume.ResumedLevels.begin(),
                           Resume.ResumedLevels.end()) !=
            Resume.ResumedLevels.end())
      return std::nullopt;
    auto &Resumption = ResumeLevels[Resume.CaptureCall];
    Resumption.Levels.insert(Resume.ResumedLevels.begin(),
                             Resume.ResumedLevels.end());
    Resumption.StateInFrame &= Resume.StateInFrame;
    if (Resume.CalleeVA)
      continue;
    if (Resume.EntryVA) {
      const auto &Scopes = Function.ExceptionMetadata->Registration->Scopes;
      Work += Scopes.size() + States.SEHControls.size();
      const auto State = BlockStates.find(Call->Block);
      if (Work > limits::kMaxRegistrationEHStateWork ||
          *Resume.ValueOffset > -4 || State == BlockStates.end() ||
          State->second->Unknown || !State->second->CallbackOnly ||
          !Blocks.count(Resume.EntryVA) ||
          !llvm::any_of(Scopes, [&](const auto &Scope) {
            return (Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA) ==
                   Resume.EntryVA;
          }))
        return std::nullopt;
      if (Resume.OrdinaryCall) {
        const auto Found = Result.find(Resume.OrdinaryCall);
        auto *Caller =
            Found == Result.end()
                ? nullptr
                : Bind(Resume.OrdinaryCall, Found->second.Operation->OriginSeq);
        if (!Caller ||
            Caller->Operation->Inputs[0].ConstVal != Resume.EntryVA ||
            !llvm::any_of(States.SEHControls, [&](const auto &Control) {
              return Control.Control ==
                         RegistrationSEHControlContract::Kind::NearCall &&
                     Control.Address == Resume.OrdinaryCall &&
                     Control.Target == Resume.EntryVA;
            }))
          return std::nullopt;
      }
    } else if (Resume.OrdinaryCall)
      return std::nullopt;
    // One instruction can run through ordinary and runtime finally entries.
    // Every invocation must agree on the native buffer and argument projection.
    if (Call->Resume) {
      auto Previous = *Call->Resume;
      if (Previous.OrdinaryCall == Resume.OrdinaryCall &&
          (Previous.ResumedLevels != Resume.ResumedLevels ||
           Previous.StateInFrame != Resume.StateInFrame))
        return std::nullopt;
      Previous.OrdinaryCall = Resume.OrdinaryCall;
      Previous.ResumedLevels = Resume.ResumedLevels;
      Previous.StateInFrame = Resume.StateInFrame;
      if (Previous != Resume)
        return std::nullopt;
      continue;
    }
    Call->Capture = Capture->second;
    Call->Resume = &Resume;
  }
  std::erase_if(Result, [](const auto &Item) { return !Item.second.Capture; });
  for (auto &[Address, Call] : Result) {
    const auto Level = ResumeLevels.find(Call.Capture->Address);
    if (Level == ResumeLevels.end()) {
      Call.ResumeLevel = Call.Capture->Level;
    } else if (Level->second.Levels.size() == 1) {
      Call.ResumeLevel = *Level->second.Levels.begin();
    } else {
      const auto &Chain = *Function.ExceptionMetadata->Registration;
      if (!Level->second.StateInFrame || !Chain.SeededTryLevel ||
          llvm::any_of(Level->second.Levels, [&](int32_t State) {
            return State != *Chain.SeededTryLevel &&
                   (State < 0 || size_t(State) >= Chain.Scopes.size());
          }))
        return std::nullopt;
      Call.ResumeLevel.reset();
    }
  }
  return Result;
}

} // namespace neverd
