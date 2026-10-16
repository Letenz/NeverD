//===- LinuxPriority.cpp - Explicit task scheduling priorities -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxPriority.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <limits>

namespace neverd::emulation::linux_model {
llvm::Error validatePriorityOptions(const LinuxPriorityOptions &Options) {
  if (Options.Tasks.size() > PriorityTaskLimit ||
      Options.RlimitNice > PriorityLimitMaximum)
    return failure(PriorityOptions);
  for (const auto &[Task, Nice] : Options.Tasks)
    if (!Task || Task > std::numeric_limits<int32_t>::max() ||
        Nice < -int32_t(PriorityNiceMagnitude) ||
        Nice > int32_t(PriorityNiceMaximum))
      return failure(PriorityOptions);
  return llvm::Error::success();
}

llvm::Error LinuxPriority::inherit(uint64_t Parent, uint64_t Child) {
  if (!Options)
    return llvm::Error::success();
  if (!Child || Child > std::numeric_limits<int32_t>::max() ||
      Tasks.contains(Child) || Exited.contains(Child)) {
    const auto Text = llvm::formatv(PriorityThreadConflict, Child).str();
    return failure(Text.c_str());
  }
  if (Tasks.size() + Exited.size() >= PriorityTaskLimit)
    return failure(PriorityThreadLimit);
  const auto It = Tasks.find(Parent);
  Tasks.emplace(Child, It == Tasks.end() ? std::nullopt : It->second);
  return llvm::Error::success();
}

void LinuxPriority::exit(uint64_t Child) {
  if (Tasks.erase(Child))
    Exited.insert(Child);
}

std::optional<uint64_t> LinuxPriority::unsupported(ProcessResult &Result,
                                                   llvm::StringRef Text) {
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = Text.str();
  return std::nullopt;
}

std::optional<uint64_t> LinuxPriority::handle(ServiceKind Kind,
                                              const ProcessServiceEvent &Event,
                                              uint64_t CurrentTask,
                                              ProcessResult &Result) {
  assert(Kind == ServiceKind::GetPriority || Kind == ServiceKind::SetPriority);
  if (!Options)
    return unsupported(Result, PriorityInputsMissing);
  const uint32_t Which = Event.Arguments[0];
  if (Which > PriorityUser)
    return uint64_t(0) - InvalidArgument;
  if (Which != PriorityProcess)
    return unsupported(Result, PrioritySelection);
  uint64_t Task = uint32_t(Event.Arguments[1]);
  if (!Task)
    Task = CurrentTask;
  auto It = Task <= std::numeric_limits<uint32_t>::max()
                ? Tasks.find(uint32_t(Task))
                : Tasks.end();
  if (It == Tasks.end() && Exited.contains(Task))
    return uint64_t(0) - NoSuchProcess;
  if (It == Tasks.end() || !It->second)
    return unsupported(Result, llvm::formatv(PriorityTaskMissing, Task).str());
  if (Kind == ServiceKind::GetPriority)
    // Raw kernel ABI, not libc's translated nice value.
    return uint64_t(int32_t(PriorityNiceMagnitude) - *It->second);
  const uint32_t Bits = Event.Arguments[2];
  const int64_t Signed = Bits <= std::numeric_limits<int32_t>::max()
                             ? int64_t(Bits)
                             : int64_t(Bits) - (int64_t(1) << 32);
  const int32_t Nice = std::clamp(Signed, -int64_t(PriorityNiceMagnitude),
                                  int64_t(PriorityNiceMaximum));
  const int32_t LowestAllowed =
      int32_t(PriorityNiceMagnitude) - int32_t(Options->RlimitNice);
  if (Nice < *It->second && !Options->CapSysNice && Nice < LowestAllowed)
    return uint64_t(0) - AccessDenied;
  It->second = Nice;
  return uint64_t(0);
}
} // namespace neverd::emulation::linux_model
