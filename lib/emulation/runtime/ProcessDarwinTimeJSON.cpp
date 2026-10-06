//===- ProcessDarwinTimeJSON.cpp - Lossless Darwin time inputs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessDarwinTimeJSON.h"

#include "../os/darwin/kernel/DarwinTime.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::DarwinTimeOptions + Name);
}
} // namespace
llvm::Expected<DarwinTimeOptions>
darwinTimeOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::DarwinTime);
  DarwinTimeOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::TimeOfDay) {
      const auto *T = V.getAsObject();
      if (!T || T->size() != 2 || !T->get(field::Seconds) ||
          !T->get(field::Microseconds))
        return invalid(Name);
      auto Seconds = process_json::integer<uint32_t>(*T->get(field::Seconds));
      auto Microseconds =
          process_json::integer<uint32_t>(*T->get(field::Microseconds));
      if (!Seconds || !Microseconds)
        return invalid(Name);
      Out.TimeOfDay = {*Seconds, *Microseconds};
    } else if (Name == field::Timezone) {
      const auto *T = V.getAsObject();
      if (!T || T->size() != 2 || !T->get(field::MinutesWest) ||
          !T->get(field::DSTTime))
        return invalid(Name);
      auto West = process_json::integer<int32_t>(*T->get(field::MinutesWest));
      auto DST = process_json::integer<int32_t>(*T->get(field::DSTTime));
      if (!West || !DST)
        return invalid(Name);
      Out.Timezone = {*West, *DST};
    } else if (Name == field::Timebase) {
      const auto *T = V.getAsObject();
      if (!T || T->size() != 2 || !T->get(field::Numerator) ||
          !T->get(field::Denominator))
        return invalid(Name);
      auto Numerator =
          process_json::integer<uint32_t>(*T->get(field::Numerator));
      auto Denominator =
          process_json::integer<uint32_t>(*T->get(field::Denominator));
      if (!Numerator || !Denominator)
        return invalid(Name);
      Out.Timebase = {*Numerator, *Denominator};
    } else if (Name == field::MachAbsoluteTime ||
               Name == field::MachContinuousTime) {
      auto T = process_json::integer<uint64_t>(V);
      if (!T)
        return invalid(Name);
      (Name == field::MachAbsoluteTime ? Out.MachAbsoluteTime
                                       : Out.MachContinuousTime) = *T;
    } else {
      return invalid(Name);
    }
  }
  if (auto E = darwin_model::validateTimeOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
