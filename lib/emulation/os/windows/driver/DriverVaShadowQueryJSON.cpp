//===- DriverVaShadowQueryJSON.cpp - Strict mitigation query declarations
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict mitigation query declarations.
///
//===----------------------------------------------------------------------===//

#include "DriverVaShadowQueryJSON.h"

#include "../kernel/KernelVaShadowQuery.h"

namespace neverd::emulation::driver_va_shadow_query {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver kernel_va_shadow_query: " + Text);
}
llvm::Expected<uint32_t> word(const llvm::json::Object &Object,
                              llvm::StringRef Name) {
  const auto *Field = Object.get(Name);
  auto Value = Field ? Field->getAsUINT64() : std::nullopt;
  if (!Value || *Value > UINT32_MAX)
    return invalid(Name + " must be a 32-bit unsigned integer");
  return uint32_t(*Value);
}
} // namespace

llvm::Expected<DriverKernelVaShadowQuery>
parse(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("declaration must be an object");
  for (const auto &[Key, Unused] : *Object)
    if (Key != "version" && Key != "flags" && Key != "evidence")
      return invalid("unknown declaration field " + Key.str());
  auto Version = word(*Object, "version");
  if (!Version)
    return Version.takeError();
  auto Flags = word(*Object, "flags");
  if (!Flags)
    return Flags.takeError();
  auto Evidence = Object->getString("evidence");
  if (!Evidence)
    return invalid("evidence must be a string");
  DriverKernelVaShadowQuery Query{*Version, *Flags, Evidence->str()};
  if (auto E = kernel_va_shadow_query::validate(Query))
    return std::move(E);
  return Query;
}

llvm::json::Object toJSON(const DriverKernelVaShadowQuery &Query) {
  return llvm::json::Object{{"version", Query.Version},
                            {"flags", Query.Flags
                                          ? llvm::json::Value(*Query.Flags)
                                          : llvm::json::Value(nullptr)},
                            {"evidence", Query.Evidence}};
}
} // namespace neverd::emulation::driver_va_shadow_query
