//===- ProcessLinuxKernelJSON.cpp - Explicit absent interfaces
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxKernelJSON.h"

#include "../os/linux/kernel/LinuxKernelAvailability.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"
namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::LinuxKernelOptions + Name);
}
std::optional<LinuxUnavailableSyscall> unavailableCall(llvm::StringRef Name) {
  struct Binding {
    llvm::StringRef Label;
    LinuxUnavailableSyscall Kind;
  };
  static constexpr Binding Calls[] = {
#define NEVERD_LINUX_UNAVAILABLE_SYSCALL(Name, Label, X64, ARM, Count)         \
  {Label, LinuxUnavailableSyscall::Name},
#include "neverd/emulation/LinuxUnavailableSyscalls.def"
#undef NEVERD_LINUX_UNAVAILABLE_SYSCALL
  };
  for (const auto &Call : Calls)
    if (Call.Label == Name)
      return Call.Kind;
  return std::nullopt;
}
std::optional<AndroidGKIKernel> gkiKernel(llvm::StringRef Name) {
#define NEVERD_LINUX_GKI_KERNEL(Kind, Label, Flags, Import, NonLeader,         \
                                NameFirst)                                     \
  if (Name == Label)                                                           \
    return AndroidGKIKernel::Kind;
#include "neverd/emulation/LinuxGKIKernels.def"
#undef NEVERD_LINUX_GKI_KERNEL
  return std::nullopt;
}
} // namespace
llvm::Expected<LinuxKernelOptions>
linuxKernelOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->empty())
    return invalid(field::LinuxKernel);
  LinuxKernelOptions Out;
  for (const auto &[Key, Entry] : *Object) {
    if (Key == field::GKI) {
      auto Name = Entry.getAsString();
      if (!Name || !(Out.GKI = gkiKernel(*Name)))
        return invalid(field::GKI);
    } else if (Key == field::KernelTasks) {
      const auto *Tasks = Entry.getAsArray();
      if (!Tasks || Tasks->size() > linux_model::KernelTaskLimit)
        return invalid(field::KernelTasks);
      auto &Catalogue = Out.Tasks.emplace();
      for (const auto &Value : *Tasks) {
        const auto *Task = Value.getAsObject();
        if (!Task || Task->size() != 2 || !Task->get(field::KernelTaskID) ||
            !Task->get(field::KernelTaskGroupLeader))
          return invalid(field::KernelTasks);
        auto ID =
            process_json::integer<uint32_t>(*Task->get(field::KernelTaskID));
        auto Leader = Task->getBoolean(field::KernelTaskGroupLeader);
        if (!ID || !Leader ||
            !Catalogue.emplace(*ID, LinuxKernelTask{*Leader}).second)
          return invalid(field::KernelTasks);
      }
    } else if (Key == field::UnavailableSyscalls) {
      const auto *Calls = Entry.getAsArray();
      if (!Calls)
        return invalid(field::UnavailableSyscalls);
      for (const auto &Call : *Calls) {
        auto Name = Call.getAsString();
        if (!Name)
          return invalid(field::UnavailableSyscalls);
        auto Kind = unavailableCall(*Name);
        if (!Kind || !Out.UnavailableSyscalls.insert(*Kind).second)
          return invalid(field::UnavailableSyscalls);
      }
    } else {
      return invalid(Key);
    }
  }
  if (auto E = linux_model::validateKernelOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
