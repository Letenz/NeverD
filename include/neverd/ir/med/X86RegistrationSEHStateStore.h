//===- X86RegistrationSEHStateStore.h - State stores ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds checked callback state transitions to their surviving MedIR stores.

#ifndef NEVERD_IR_MED_X86REGISTRATIONSEHSTATESTORE_H
#define NEVERD_IR_MED_X86REGISTRATIONSEHSTATESTORE_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {
struct MedFunc;
struct MedOp;
struct RegistrationSEHStateStore;

struct X86RegistrationSEHStateStoreBinding {
  const MedOp *Operation = nullptr;
  const RegistrationSEHStateStore *Source = nullptr;
  /// The complete source state after every reaching invocation of this store.
  /// A narrow store's literal is only the overwritten low bytes.
  int32_t TargetLevel = -1;
};

/// Return callback stores in source occurrence order. Every checked transition
/// must produce the same complete level, preserving untouched high bytes.
/// The source solver owns address and transition semantics. Native projection
/// and replay retain their ordinary checks of the current memory operand.
std::optional<std::vector<X86RegistrationSEHStateStoreBinding>>
getX86RegistrationSEHStateStores(const MedFunc &Function);

/// Return sorted scopes selected only by callback writes, including enclosing
/// scopes. Every scope must be reachable from a parent state or callback write.
std::optional<std::vector<uint32_t>>
getX86RegistrationSEHRetainedScopes(const MedFunc &Function);

} // namespace neverd

#endif
