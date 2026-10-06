//===- KernelModelPoFx.cpp - Guest PoFx callback execution ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error poFxError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PoFx: " + Message);
}
} // namespace

llvm::Error KernelModel::queuePoFxCallbacks() {
  while (auto Call = PoFx.nextCallback()) {
    if (Call->Internal) {
      if (auto E = PoFx.submitCallback(Call->Token))
        return E;
      if (auto E = PoFx.beginCallback(Call->Token))
        return E;
      if (auto E = processInternalPoFxCallback(*Call))
        return E;
      if (auto E = PoFx.finishCallback(Call->Token))
        return E;
      continue;
    }
    if (Call->Thread) {
      auto Operation = BlockingPoFx.find(Call->Thread);
      if (Operation == BlockingPoFx.end())
        return poFxError("same-thread callback lost its blocking operation");
      if (auto E = PoFx.submitCallback(Call->Token))
        return E;
      const auto TerminalKind = Operation->second.Active
                                    ? KernelPoFx::CallbackKind::ActiveCondition
                                    : KernelPoFx::CallbackKind::IdleCondition;
      if (Call->Kind == TerminalKind)
        Operation->second.TerminalCallback = Call->Token;
      Operation->second.Calls.push_back(
          {{GuestCallOwner::PoFx, Call->Token}, Call->PC, Call->Arguments});
      continue;
    }
    const auto Owner = PoFxDeviceObjects.find(Call->Handle);
    if (Owner == PoFxDeviceObjects.end())
      return poFxError("callback lost its device-object reference");
    KernelScheduler::Callback Invocation;
    Invocation.Object = Call->Token;
    Invocation.Owner = Owner->second;
    Invocation.Thread = profile::WorkerThreadIdentity;
    Invocation.PC = Call->PC;
    Invocation.Arguments = Call->Arguments;
    auto ID = Scheduler.enqueuePoFx(std::move(Invocation));
    if (!ID)
      return ID.takeError();
    if (auto E = PoFx.submitCallback(Call->Token))
      return E;
    ScheduledModelContinuations.emplace(
        *ID, GuestCallToken{GuestCallOwner::PoFx, Call->Token});
  }
  for (auto &[Thread, Operation] : BlockingPoFx) {
    auto Component = PoFx.component(Operation.Handle, Operation.Component);
    if (!Component)
      return Component.takeError();
    const uint64_t Generation = Operation.Active ? Component->ActiveGeneration
                                                 : Component->IdleGeneration;
    if (Generation != Operation.CompletionGeneration ||
        (Operation.TerminalCallback &&
         !PoFx.callback(*Operation.TerminalCallback)))
      Operation.Completed = true;
  }
  return llvm::Error::success();
}

std::optional<KernelGuestCall>
KernelModel::takePoFxThreadCall(uint64_t Thread) {
  auto Operation = BlockingPoFx.find(Thread);
  if (Operation == BlockingPoFx.end() || Operation->second.Calls.empty())
    return std::nullopt;
  auto Call = std::move(Operation->second.Calls.front());
  Operation->second.Calls.pop_front();
  return Call;
}

llvm::Error KernelModel::waitForPoFxOperation(uint64_t Thread) {
  auto It = BlockingPoFx.find(Thread);
  if (It == BlockingPoFx.end())
    return poFxError("blocking continuation lost its caller");
  const auto &Operation = It->second;
  if (!Operation.Calls.empty())
    return llvm::Error::success();
  auto Ready = PoFx.conditionReached(Operation.Handle, Operation.Component,
                                     Operation.Active);
  if (!Ready)
    return Ready.takeError();
  if (Operation.Completed || *Ready) {
    BlockingPoFx.erase(It);
    return llvm::Error::success();
  }
  if (PendingWait)
    return poFxError("blocking continuation cannot replace a wait");
  PendingWait =
      Wait{Operation.Active ? Wait::Kind::PoFxActive : Wait::Kind::PoFxIdle,
           Operation.Handle, CurrentExecution, Thread, CurrentIRQL};
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishPoFxCall(uint64_t Token) {
  const auto *Call = PoFx.callback(Token);
  if (!Call)
    return poFxError("callback return requires a live invocation");
  const uint64_t Thread = Call->Thread;
  if (auto E = PoFx.finishCallback(Token))
    return E;
  if (auto E = queuePoFxCallbacks())
    return E;
  if (Framework)
    if (auto E = Framework->resumePoFxTransitions())
      return E;
  if (Thread) {
    if (auto E = waitForPoFxOperation(Thread))
      return E;
    auto Operation = BlockingPoFx.find(Thread);
    if (Operation != BlockingPoFx.end() && !Operation->second.Calls.empty())
      return std::optional<uint64_t>{};
  }
  return std::optional<uint64_t>{0};
}
} // namespace neverd::emulation
