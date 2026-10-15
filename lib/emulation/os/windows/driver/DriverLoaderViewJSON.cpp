//===- DriverLoaderViewJSON.cpp - Loader view scenario transport ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Parses and records declarations without granting them native authority.
//===----------------------------------------------------------------------===//

#include "DriverLoaderViewJSON.h"

#include "../kernel/KernelLoaderView.h"

namespace neverd::emulation::driver_loader_view {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver loader_view: " + Text);
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

llvm::Expected<DriverLoaderView> parse(const llvm::json::Value &Value) {
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
  DriverLoaderView View{*Version, *Flags, Evidence->str()};
  if (auto E = kernel_loader_view::validate(View))
    return std::move(E);
  return View;
}

llvm::json::Object toJSON(const DriverLoaderView &View) {
  return llvm::json::Object{{"version", View.Version},
                            {"flags", View.Flags
                                          ? llvm::json::Value(*View.Flags)
                                          : llvm::json::Value(nullptr)},
                            {"evidence", View.Evidence}};
}
} // namespace neverd::emulation::driver_loader_view
