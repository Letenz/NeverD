//===- DriverSchedulingJSON.cpp - Explicit virtual scheduling policy -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DriverSchedulingJSON.h"

#include <limits>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Error validateDriverScheduling(const DriverScheduling &Policy) {
  if (!Policy.QuantumInstructions || !Policy.InstructionTime100ns)
    return invalid(driver_scheduling::InvalidValue);
  if (Policy.QuantumInstructions >
      std::numeric_limits<uint64_t>::max() / Policy.InstructionTime100ns)
    return invalid(driver_scheduling::InvalidDuration);
  return llvm::Error::success();
}

llvm::Expected<DriverScheduling>
parseDriverScheduling(const llvm::json::Value &Value) {
  const auto *Object = Value.getAsObject();
  if (!Object)
    return invalid(driver_scheduling::InvalidObject);
  DriverScheduling Policy;
  for (const auto &[Name, Entry] : *Object) {
    uint64_t *Destination = nullptr;
    if (Name == driver_scheduling::QuantumInstructions)
      Destination = &Policy.QuantumInstructions;
    else if (Name == driver_scheduling::InstructionTime100ns)
      Destination = &Policy.InstructionTime100ns;
    else
      return invalid(driver_scheduling::InvalidFields);
    auto Number = Entry.getAsUINT64();
    if (!Number || !*Number)
      return invalid(driver_scheduling::InvalidValue);
    *Destination = *Number;
  }
  if (auto E = validateDriverScheduling(Policy))
    return std::move(E);
  return Policy;
}

llvm::json::Object driverSchedulingJSON(const DriverScheduling &Policy) {
  return llvm::json::Object{
      {driver_scheduling::QuantumInstructions, Policy.QuantumInstructions},
      {driver_scheduling::InstructionTime100ns, Policy.InstructionTime100ns}};
}
} // namespace neverd::emulation
