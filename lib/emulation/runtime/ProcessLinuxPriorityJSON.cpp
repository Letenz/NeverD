//===- ProcessLinuxPriorityJSON.cpp - Explicit nice and authority inputs --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxPriorityJSON.h"

#include "../os/linux/kernel/LinuxPriority.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::LinuxPriorityOptions + Name);
}
} // namespace
llvm::Expected<LinuxPriorityOptions>
linuxPriorityOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::LinuxPriority);
  LinuxPriorityOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::PriorityTasks)
      continue;
    if (Name == field::PriorityCapSysNice) {
      auto Flag = V.getAsBoolean();
      if (!Flag)
        return invalid(Name);
      Out.CapSysNice = *Flag;
    } else if (Name == field::PriorityRlimitNice) {
      auto Limit = process_json::integer<uint32_t>(V);
      if (!Limit)
        return invalid(Name);
      Out.RlimitNice = *Limit;
    } else {
      return invalid(Name);
    }
  }
  const auto *Tasks = Object->getArray(field::PriorityTasks);
  if (!Tasks || Tasks->size() > linux_model::PriorityTaskLimit)
    return invalid(field::PriorityTasks);
  for (const auto &Entry : *Tasks) {
    const auto *Task = Entry.getAsObject();
    if (!Task || Task->size() != 2 || !Task->get(field::PriorityTaskID) ||
        !Task->get(field::PriorityNice))
      return invalid(field::PriorityTasks);
    auto ID =
        process_json::integer<uint32_t>(*Task->get(field::PriorityTaskID));
    auto Nice = process_json::integer<int32_t>(*Task->get(field::PriorityNice));
    if (!ID || !Nice || !Out.Tasks.emplace(*ID, *Nice).second)
      return invalid(field::PriorityTasks);
  }
  if (auto E = linux_model::validatePriorityOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
