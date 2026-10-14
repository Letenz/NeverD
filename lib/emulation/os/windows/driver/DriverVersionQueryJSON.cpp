//===- DriverVersionQueryJSON.cpp - Explicit concrete query declarations
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict JSON for explicit version query declarations.
///
//===----------------------------------------------------------------------===//

#include "DriverVersionQueryJSON.h"

#include "../kernel/KernelVersionQuery.h"

namespace neverd::emulation::driver_version_query {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver version_query: " + Text);
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

llvm::Expected<DriverVersionQuery> parse(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("declaration must be an object");
  for (const auto &[Key, Unused] : *Object)
    if (Key != "version" && Key != "build_number" && Key != "evidence" &&
        Key != "allow_zero_size")
      return invalid("unknown declaration field " + Key.str());
  auto Version = word(*Object, "version");
  if (!Version)
    return Version.takeError();
  auto Build = word(*Object, "build_number");
  if (!Build)
    return Build.takeError();
  auto Evidence = Object->getString("evidence");
  if (!Evidence)
    return invalid("evidence must be a string");
  DriverVersionQuery Query{*Version, *Build, Evidence->str(), false};
  if (const auto *Zero = Object->get("allow_zero_size")) {
    auto Boolean = Zero->getAsBoolean();
    if (!Boolean)
      return invalid("allow_zero_size must be boolean");
    Query.AllowZeroSize = *Boolean;
  }
  if (auto E = kernel_version_query::validate(Query))
    return std::move(E);
  return Query;
}

llvm::json::Object toJSON(const DriverVersionQuery &Query) {
  return llvm::json::Object{
      {"version", Query.Version},
      {"build_number", Query.BuildNumber ? llvm::json::Value(*Query.BuildNumber)
                                         : llvm::json::Value(nullptr)},
      {"evidence", Query.Evidence},
      {"allow_zero_size", Query.AllowZeroSize}};
}
} // namespace neverd::emulation::driver_version_query
