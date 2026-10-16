//===- PrototypeDebugContext.h - Signatures from a type library -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A program's debug information with the signatures it lacks taken from
/// its local types: a PDB of public symbols names NtCreateFile but gives it
/// no type, where the Windows kernel's headers declare its prototype.  Only
/// a function the debug information names and leaves untyped takes one; a
/// typed declaration of the program's own stays as it is.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_PROTOTYPEDEBUGCONTEXT_H
#define NEVERD_SDK_CAPI_PROTOTYPEDEBUGCONTEXT_H

#include "neverd/debug/DebugContext.h"
#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <memory>
#include <mutex>

namespace neverd::sdk {

/// \p Type as the decompiler's types describe it: records by their name with
/// their members' names for display, typedefs as what they name, pointers
/// to functions as pointers.  Null for a type it cannot describe, such as a
/// function by value or a record larger than a display offset reaches.
TypeRef decompilerTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type);

/// The names of the types the decompiler reads with \p Library's
/// prototypes: those their parameters and returns name, and those these
/// name in turn, through typedefs, pointers, arrays and members, whether
/// the library defines them or not.  A change to another type leaves what
/// the prototypes say alone.
llvm::StringSet<> prototypeTypeNames(const types::TypeLibrary &Library);

class PrototypeDebugContext final : public DebugContext {
public:
  /// \p Inner outlives this; \p Types holds the prototypes and the types
  /// they name.
  PrototypeDebugContext(const DebugContext &Inner,
                        std::shared_ptr<const types::TypeLibrary> Types);

  /// The inner function, its return type and parameters the library's
  /// prototype of its name where the inner context gives neither.
  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;

  std::optional<std::string> functionName(va_t Addr) const override {
    return Inner.functionName(Addr);
  }
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override {
    return Inner.resolveVariable(FuncAddr, Offset);
  }
  bool hasAuthenticatedFunctionSignatures() const override {
    return Inner.hasAuthenticatedFunctionSignatures();
  }
  AuthenticatedReturnValueState
  resolveAuthenticatedReturnValueState(va_t Addr) const override {
    return Inner.resolveAuthenticatedReturnValueState(Addr);
  }
  bool hasAuthenticatedObjectExtents() const override {
    return Inner.hasAuthenticatedObjectExtents();
  }
  std::vector<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordParameters(va_t Addr) const override {
    return Inner.resolveAuthenticatedRecordParameters(Addr);
  }
  std::optional<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordReturn(va_t Addr) const override {
    return Inner.resolveAuthenticatedRecordReturn(Addr);
  }
  VariableExtentLookup resolveVariableAt(va_t FuncAddr, va_t UsePC,
                                         int64_t Offset) const override {
    return Inner.resolveVariableAt(FuncAddr, UsePC, Offset);
  }
  std::optional<VariableSym>
  resolveStackPointerVariable(va_t FuncAddr, int64_t Offset) const override {
    return Inner.resolveStackPointerVariable(FuncAddr, Offset);
  }
  VariableExtentLookup
  resolveStackPointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const override {
    return Inner.resolveStackPointerVariableAt(FuncAddr, UsePC, Offset);
  }
  VariableExtentLookup
  resolveFramePointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const override {
    return Inner.resolveFramePointerVariableAt(FuncAddr, UsePC, Offset);
  }
  /// Records this context made carry their members already.
  void completeType(const TypeRef &Type) const override {
    Inner.completeType(Type);
  }
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override {
    return Inner.resolveType(TypeId);
  }
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override {
    return Inner.sourceLocation(Addr);
  }
  std::vector<FunctionSym> allFunctions() const override {
    return Inner.allFunctions();
  }
  std::vector<DataObjectSym> allDataObjects() const override {
    return Inner.allDataObjects();
  }
  std::optional<DataObjectSym> resolveDataObject(va_t Addr) const override {
    return Inner.resolveDataObject(Addr);
  }
  std::pair<std::optional<va_t>, std::optional<va_t>>
  dataSymbolsAround(va_t Addr) const override {
    return Inner.dataSymbolsAround(Addr);
  }
  std::optional<va_t> precedingDataSymbol(va_t Addr) const override {
    return Inner.precedingDataSymbol(Addr);
  }
  bool hasInfo() const override { return Inner.hasInfo(); }

  /// The signature the library gives \p Name: its return type and named
  /// parameters.  None for a name without a prototype, a variadic one,
  /// whose calls pass more than it declares, or one with a type the
  /// decompiler cannot describe.
  std::optional<FunctionSym> prototype(llvm::StringRef Name) const;

private:
  const DebugContext &Inner;
  std::shared_ptr<const types::TypeLibrary> Types;
  mutable std::mutex Mutex;
  mutable llvm::StringMap<std::optional<FunctionSym>> Signatures;
};

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_PROTOTYPEDEBUGCONTEXT_H
