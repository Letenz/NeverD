//===- ProcessLinuxSignalsJSON.cpp - Explicit signal action inputs -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxSignalsJSON.h"

#include "../os/linux/kernel/LinuxSignals.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::LinuxSignalsOptions + Name);
}
} // namespace

llvm::Expected<LinuxSignalOptions>
linuxSignalOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object || Object->size() != 1)
    return invalid(field::LinuxSignals);
  const auto *Actions = Object->getArray(field::SignalActions);
  if (!Actions || Actions->size() > linux_model::SignalCount)
    return invalid(field::SignalActions);
  LinuxSignalOptions Out;
  for (const auto &Entry : *Actions) {
    const auto *Action = Entry.getAsObject();
    if (!Action || Action->size() != 5 || !Action->get(field::SignalNumber))
      return invalid(field::SignalActions);
    auto Signal =
        process_json::integer<int32_t>(*Action->get(field::SignalNumber));
    if (!Signal)
      return invalid(field::SignalNumber);
    LinuxSignalAction Decoded;
    struct Field {
      const char *Name;
      uint64_t &Destination;
    };
    for (const Field &F : {Field{field::SignalHandler, Decoded.Handler},
                           Field{field::SignalFlags, Decoded.Flags},
                           Field{field::SignalRestorer, Decoded.Restorer},
                           Field{field::SignalMask, Decoded.Mask}}) {
      const auto *V = Action->get(F.Name);
      if (!V)
        return invalid(F.Name);
      auto Number = process_json::integer<uint64_t>(*V);
      if (!Number)
        return invalid(F.Name);
      F.Destination = *Number;
    }
    if (!Out.Actions.emplace(*Signal, Decoded).second)
      return invalid(field::SignalNumber);
  }
  if (auto E = linux_model::validateSignalOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
