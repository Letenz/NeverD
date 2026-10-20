//===- SourceVariableTypes.h - Analyst source variable types --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_SOURCEVARIABLETYPES_H
#define NEVERD_BACKEND_C_SOURCEVARIABLETYPES_H

#include "neverd/ir/NdTypes.h"
#include "neverd/ir/SourceVariableIdentity.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// An emitter-owned source variable. Id names an actual IR value or a group
/// the IR has already coalesced; Name is its current C spelling. The enclosing
/// function/route snapshot authenticates Id only for that source projection.
/// This is neither debug authentication nor native storage/ABI authority.
struct SourceVariable {
  va_t Function = 0;
  va_t Address = 0;
  std::string Id;
  std::string Name;
  uint16_t Bytes = 0;
  TypeRef Type;
  std::optional<int64_t> StackOffset;
  bool Parameter = false;
  bool Ambiguous = false;
};

/// Checked, source-only assignments. A caller validates the persisted snapshot
/// before constructing these bindings. A backend still checks the value width
/// at every use; no type grants a new memory access or changes machine bits.
struct SourceVariableTypes {
  std::map<std::pair<va_t, std::string>, TypeRef> Locals;
  std::map<va_t, TypeRef> Globals;

  TypeRef local(va_t Function, const std::string &Id, uint16_t Bytes) const {
    const auto It = Locals.find({Function, Id});
    return It != Locals.end() && compatible(It->second, Bytes) ? It->second
                                                               : nullptr;
  }
  TypeRef global(va_t Address, uint16_t Bytes) const {
    const auto It = Globals.find(Address);
    return It != Globals.end() && compatible(It->second, Bytes) ? It->second
                                                                : nullptr;
  }
  static bool compatible(const TypeRef &Type, uint16_t Bytes) {
    return Type && Bytes && Type->Size == Bytes &&
           (Type->Kind == NdTypeKind::Int || Type->Kind == NdTypeKind::Float ||
            Type->Kind == NdTypeKind::Ptr ||
            (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
  }
};

/// Record every binding of a displayed variable. A source name or coalesced
/// identity covering incompatible widths is explicitly ineligible for typing.
using SourceVariableInventory =
    std::map<std::pair<va_t, std::string>, SourceVariable>;

inline void recordSourceVariable(SourceVariableInventory &Variables,
                                 SourceVariable Variable) {
  const auto Key = std::make_pair(Variable.Function, Variable.Id);
  if (auto It = Variables.find(Key); It != Variables.end()) {
    auto &Existing = It->second;
    Existing.Ambiguous |=
        Variable.Ambiguous || Existing.Bytes != Variable.Bytes;
    Existing.Name = Variable.Name;
    if (!Existing.Type)
      Existing.Type = Variable.Type;
    return;
  }
  Variables.emplace(Key, std::move(Variable));
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_SOURCEVARIABLETYPES_H
