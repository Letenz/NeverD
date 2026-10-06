//===- ProcessDarwinSystemJSON.cpp - Explicit system inputs ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessDarwinSystemJSON.h"

#include "../os/darwin/kernel/DarwinSystem.h"
#include "ProcessJSONInteger.h"

#include "neverd/emulation/ProcessReportFields.h"

#include <type_traits>

namespace neverd::emulation {
namespace {
namespace field = process_report;
llvm::Error invalid(llvm::StringRef Name) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 field::DarwinSystemOptions + Name);
}
template <typename T>
bool parse(const llvm::json::Value &Value, std::optional<T> &Out) {
  if constexpr (std::is_same_v<T, std::string>) {
    auto Text = Value.getAsString();
    if (!Text)
      return false;
    Out = Text->str();
  } else {
    Out = process_json::integer<T>(Value);
  }
  return Out.has_value();
}
} // namespace
llvm::Expected<DarwinSystemOptions>
darwinSystemOptionsFromJSON(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(field::DarwinSystem);
  DarwinSystemOptions Out;
  for (const auto &[Key, V] : *Object) {
    const llvm::StringRef Name = Key;
    if (Name == field::SystemResourceLimits) {
      const auto *Limits = V.getAsArray();
      if (!Limits || Limits->size() > darwin_model::value::ResourceLimitCount)
        return invalid(Name);
      for (const auto &Entry : *Limits) {
        const auto *Limit = Entry.getAsObject();
        if (!Limit || Limit->size() != 3)
          return invalid(Name);
        const auto *R = Limit->get(field::ResourceLimitResource);
        const auto *C = Limit->get(field::ResourceLimitCurrent);
        const auto *M = Limit->get(field::ResourceLimitMaximum);
        if (!R || !C || !M)
          return invalid(Name);
        auto Resource = process_json::integer<uint32_t>(*R);
        auto Current = process_json::integer<uint64_t>(*C);
        auto Maximum = process_json::integer<uint64_t>(*M);
        if (!Resource || !Current || !Maximum ||
            !Out.ResourceLimits
                 .emplace(*Resource, DarwinResourceLimit{*Current, *Maximum})
                 .second)
          return invalid(Name);
      }
      continue;
    }
#define NEVERD_DARWIN_SYSTEM_FIELD(Member, Field, NativeName, Root, Leaf)      \
  if (Name == field::Field) {                                                  \
    if (!parse(V, Out.Member))                                                 \
      return invalid(Name);                                                    \
    continue;                                                                  \
  }
#include "../os/darwin/kernel/DarwinSystemFields.def"
#undef NEVERD_DARWIN_SYSTEM_FIELD
    return invalid(Name);
  }
  if (auto E = darwin_model::validateSystemOptions(Out))
    return std::move(E);
  return Out;
}
} // namespace neverd::emulation
