//===- COFFRegistrationCopySlots.h - Recovered scalar storage ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONCOPYSLOTS_H
#define NEVERD_COFFREGISTRATIONCOPYSLOTS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <map>
#include <vector>

namespace llvm {
class AllocaInst;
class Function;
class IntrinsicInst;
class Type;
class Value;
} // namespace llvm

namespace neverd::coff_registration {
/// Resolve outlined copies back to their exact parent allocation. Every alias
/// remains subject to the same scalar-only use closure as a local spill slot.
class RegistrationCopySlots {
public:
  bool build(const llvm::Function &Parent,
             llvm::ArrayRef<const llvm::Function *> Callbacks, size_t &Work);
  bool isPrivate(const llvm::Value *Value, const llvm::Type *Type, size_t &Work,
                 bool AllowVolatile) const;
  bool isStorage(const llvm::Value *Value) const;
  bool isEscape(const llvm::Value *Value) const;
  llvm::SmallVector<const llvm::Value *, 4>
  aliases(const llvm::Value *Value) const;

private:
  const llvm::AllocaInst *allocation(const llvm::Value *Value) const;
  const llvm::IntrinsicInst *Escape = nullptr;
  std::map<const llvm::Value *, const llvm::AllocaInst *> Recovered;
  std::map<const llvm::AllocaInst *, std::vector<const llvm::Value *>> Aliases;
};
} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONCOPYSLOTS_H
