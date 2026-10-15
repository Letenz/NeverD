//===- DriverPhysicalInventoryJSON.cpp - Explicit inventory declarations --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit inventory declarations.
///
//===----------------------------------------------------------------------===//
#include "DriverPhysicalInventoryJSON.h"

#include "../kernel/KernelPhysicalInventory.h"

namespace neverd::emulation::driver_physical_inventory {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError("driver physical_memory_inventory: " + Text);
}
} // namespace

llvm::Expected<DriverPhysicalMemoryInventory>
parse(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid("declaration must be an object");
  for (const auto &[Key, Unused] : *Object)
    if (Key != "version" && Key != "source" && Key != "evidence")
      return invalid("unknown declaration field " + Key.str());
  const auto *Field = Object->get("version");
  const auto Version = Field ? Field->getAsUINT64() : std::nullopt;
  if (!Version || *Version > UINT32_MAX)
    return invalid("version must be a 32-bit unsigned integer");
  const auto Source = Object->getString("source");
  if (Source != "model-identity-window")
    return invalid("source must select model-identity-window");
  const auto Evidence = Object->getString("evidence");
  if (!Evidence)
    return invalid("evidence must be a string");
  DriverPhysicalMemoryInventory Inventory{
      uint32_t(*Version),
      DriverPhysicalMemoryInventory::SourceKind::ModelIdentityWindow,
      Evidence->str()};
  if (auto E = kernel_physical_inventory::validate(Inventory))
    return std::move(E);
  return Inventory;
}

llvm::json::Object toJSON(const DriverPhysicalMemoryInventory &Inventory) {
  return llvm::json::Object{
      {"version", Inventory.Version},
      {"source",
       Inventory.Source ==
               DriverPhysicalMemoryInventory::SourceKind::ModelIdentityWindow
           ? llvm::json::Value("model-identity-window")
           : llvm::json::Value(nullptr)},
      {"evidence", Inventory.Evidence}};
}
} // namespace neverd::emulation::driver_physical_inventory
