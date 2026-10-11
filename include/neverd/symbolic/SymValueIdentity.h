//===- SymValueIdentity.h - Exact identities of runtime values -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Optional provenance for fresh runtime results, independent of ISA and OS.

#ifndef NEVERD_SYMBOLIC_SYMVALUEIDENTITY_H
#define NEVERD_SYMBOLIC_SYMVALUEIDENTITY_H

#include "neverd/symbolic/SymExpr.h"

#include <map>
#include <string>

namespace neverd::symbolic {

struct SymValueIdentity {
  enum class Kind { Symbol, Module };
  Kind TheKind = Kind::Symbol;
  std::string Name;
  std::string Module;
  std::string Resolver;
  uint64_t Function = 0;
  uint64_t Address = 0;
  bool operator==(const SymValueIdentity &) const = default;
};

/// Context-local identities. These name a resolver's result, which can still
/// be null; they neither replace it with an address nor assert successful
/// resolution. Shared SymState owns forwarding through registers and memory.
/// Only the exact complete expression retains identity. Arithmetic, partial
/// overwrites, unknown aliasing stores and opaque calls must not inherit it
/// merely because their expressions depend on a named result.
class SymValueIdentities {
public:
  bool bind(const SymContext &Context, SymRef Value,
            const SymValueIdentity &Identity) {
    if (!Value || Identity.Name.empty() || !Context.isVar(Value) ||
        !Context.varInfo(Context.varId(Value)).Fresh ||
        (Context.width(Value) != 32 && Context.width(Value) != 64))
      return false;
    auto [At, Added] = Values.emplace(Value, Identity);
    if (!Added && At->second != Identity) {
      Values.erase(At);
      Conflicts.emplace(Value, true);
    }
    return !Conflicts.count(Value);
  }

  const SymValueIdentity *lookup(SymRef Value) const {
    if (Conflicts.count(Value))
      return nullptr;
    auto At = Values.find(Value);
    return At == Values.end() ? nullptr : &At->second;
  }

private:
  std::map<SymRef, SymValueIdentity> Values;
  std::map<SymRef, bool> Conflicts;
};

} // namespace neverd::symbolic
#endif
