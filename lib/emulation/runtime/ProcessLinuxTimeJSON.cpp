//===- ProcessLinuxTimeJSON.cpp - Lossless explicit clock inputs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessLinuxTimeJSON.h"

#include "../os/linux/kernel/LinuxTime.h"

#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "invalid linux_time option: " + Name);
}
llvm::Expected<int64_t> integer(const llvm::json::Value &V,
                                llvm::StringRef Name, bool Narrow = false) {
  std::optional<int64_t> Number;
  if (auto S = V.getAsString()) {
    auto Digits = *S;
    Digits.consume_front("-");
    int64_t N;
    if (!Digits.empty() && llvm::all_of(Digits, llvm::isDigit) &&
        !S->getAsInteger(10, N))
      Number = N;
  } else if (auto N = V.getAsNumber()) {
    // Keep numeric JSON safe across C and Python/JavaScript producers. Decimal
    // strings cover the full signed 64-bit domain without floating rounding.
    if (*N >= -9007199254740991.0 && *N <= 9007199254740991.0)
      Number = V.getAsInteger();
  }
  if (!Number || (Narrow && (*Number < INT32_MIN || *Number > INT32_MAX)))
    return invalid(Name);
  return *Number;
}
} // namespace
llvm::Expected<LinuxTimeOptions>
linuxTimeOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::LinuxTime);
  LinuxTimeOptions Out;
  for (const auto &[Key, V] : *Object) {
    llvm::StringRef Name = Key;
    if (Name == field::Clocks) {
      const auto *Clocks = V.getAsArray();
      if (!Clocks)
        return invalid(Name);
      for (const auto &Clock : *Clocks) {
        const auto *C = Clock.getAsObject();
        if (!C || C->size() != 3 || !C->get(field::ClockID) ||
            !C->get(field::Seconds) || !C->get(field::Nanoseconds))
          return invalid(Name);
        auto ID = integer(*C->get(field::ClockID), field::ClockID, true);
        if (!ID)
          return ID.takeError();
        auto Sec = integer(*C->get(field::Seconds), field::Seconds);
        if (!Sec)
          return Sec.takeError();
        auto NSec = integer(*C->get(field::Nanoseconds), field::Nanoseconds);
        if (!NSec)
          return NSec.takeError();
        if (!Out.Clocks.emplace(*ID, LinuxTimespec{*Sec, *NSec}).second)
          return invalid(field::ClockID);
      }
    } else if (Name == field::Timezone) {
      const auto *Zone = V.getAsObject();
      if (!Zone || Zone->size() != 2 || !Zone->get(field::MinutesWest) ||
          !Zone->get(field::DSTTime))
        return invalid(Name);
      auto West =
          integer(*Zone->get(field::MinutesWest), field::MinutesWest, true);
      if (!West)
        return West.takeError();
      auto DST = integer(*Zone->get(field::DSTTime), field::DSTTime, true);
      if (!DST)
        return DST.takeError();
      Out.Timezone = LinuxTimezone{static_cast<int32_t>(*West),
                                   static_cast<int32_t>(*DST)};
    } else {
      return invalid(Name);
    }
  }
  if (auto E = linux_model::validateTimeOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
