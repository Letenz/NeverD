//===- LocalTypesDebugContext.h - Debug info from local types ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A program's debug information completed from its local types, which are
/// the authority on its records:
/// - the signatures it lacks: a PDB of public symbols names NtCreateFile but
///   gives it no type, where the Windows kernel's headers declare its
///   prototype.  Only a function the debug information names and leaves
///   untyped takes one; a typed declaration of the program's own stays;
/// - the members of its records, which the decompiler names its accesses
///   by, so that a member the user renames reads by its new name;
/// - the C that defines those records, which a decompiled unit holds;
/// - the declarations the user gives functions, by their addresses, as
///   IDA's Y does: these come first, before the debug information's.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LOCALTYPESDEBUGCONTEXT_H
#define NEVERD_SDK_CAPI_LOCALTYPESDEBUGCONTEXT_H

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace neverd::sdk {

/// \p Type as the decompiler's types describe it: records by their name with
/// their members' names for display, typedefs as what they name, pointers
/// to functions as pointers.  Null for a type it cannot describe, such as a
/// function by value or a record larger than a display offset reaches.
TypeRef decompilerTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type);

/// \p Function's signature, named \p Name, as the decompiler's types
/// describe it.  None for a variadic one, whose calls pass more than it
/// declares, or one with a type the decompiler cannot describe.
std::optional<FunctionSym> signatureOf(const types::TypeLibrary &Library,
                                       const types::FunctionDef &Function,
                                       llvm::StringRef Name);

/// The names of the types the decompiler reads with \p Library's
/// prototypes: those their parameters and returns name, and those these
/// name in turn, through typedefs, pointers, arrays and members, whether
/// the library defines them or not.  A change to another type leaves what
/// the prototypes say alone.
llvm::StringSet<> prototypeTypeNames(const types::TypeLibrary &Library);

class LocalTypesDebugContext final : public DebugContext {
public:
  /// \p Inner outlives this; \p Types holds the prototypes and the types
  /// they name.  \p Declarations are the C declarations the user gave the
  /// functions at those addresses, which read against \p Types.
  LocalTypesDebugContext(const DebugContext &Inner,
                         std::shared_ptr<const types::TypeLibrary> Types,
                         std::map<va_t, std::string> Declarations = {});
  /// Takes back the members it gave the debug information's records, so
  /// that the context made after a change to the library gives them anew.
  /// The records are the debug information's, which the session's calls,
  /// one at a time, share: no decompilation reads them meanwhile.
  ~LocalTypesDebugContext() override;

  /// The inner function, its return type and parameters those of the
  /// user's declaration at its entry, else the library's prototype of its
  /// name where the inner context gives neither.
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
  /// A record's members for display from the library, where it defines a
  /// record of the name and size whose members none have been given yet;
  /// otherwise the debug information's.
  void completeType(const TypeRef &Type) const override;
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
  bool hasInfo() const override {
    return Inner.hasInfo() || !Declarations.empty();
  }

  /// The signature the library gives \p Name: its return type and named
  /// parameters.  None for a name without a prototype, a variadic one,
  /// whose calls pass more than it declares, or one with a type the
  /// decompiler cannot describe.
  std::optional<FunctionSym> prototype(llvm::StringRef Name) const;

  /// The signature of the user's declaration of the function at \p Entry.
  /// None where the user declared none, or one that no longer reads against
  /// the library, or has a type the decompiler cannot describe.
  std::optional<FunctionSym> declared(va_t Entry) const;

  /// The C that defines \p Records for a decompiled unit
  /// (CEmitterOptions::RecordDefinitions): each one the library defines, of
  /// the name and size the unit reads, whose members the unit names as the
  /// library does, and what these hold by value.  A record of no size or
  /// members is a name the unit spells: a record's or an enumeration's of
  /// the library's, or an enumerator of one the decompiler displays, which
  /// stands for that enumeration.  What the debug information gave types of
  /// the name, members or enumerators, must agree with the library too.
  CRecordDefinitions definitions(const std::vector<TypeRef> &Records) const;

  /// The library's types the decompiler has read through this context: the
  /// records whose members it gave and the types units held.  A change to
  /// one changes what the decompiler prints.
  llvm::StringSet<> readTypes() const;

private:
  /// The record the library defines as \p Name, of \p Size bytes unless
  /// that is unknown.
  const types::LocalType *definedRecord(llvm::StringRef Name,
                                        uint16_t Size) const;

  const DebugContext &Inner;
  std::shared_ptr<const types::TypeLibrary> Types;
  std::map<va_t, std::string> Declarations;
  mutable std::mutex Mutex;
  mutable llvm::StringMap<std::optional<FunctionSym>> Signatures;
  mutable std::map<va_t, std::optional<FunctionSym>> DeclaredSignatures;
  /// The debug information's records it gave members to.
  mutable std::vector<TypeRef> Completed;
  mutable llvm::DenseSet<const NdType *> CompletedRecords;
  mutable llvm::StringSet<> Read;
  /// The records and enumerations of the library's names that the debug
  /// information gave members or enumerators, by name.
  mutable llvm::StringMap<std::vector<TypeRef>> Given;
  mutable llvm::DenseSet<const NdType *> GivenTypes;
  /// The library's names C spells otherwise, which a unit cannot name.
  mutable std::optional<llvm::StringSet<>> RenamedInC;
};

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_LOCALTYPESDEBUGCONTEXT_H
