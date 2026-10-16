//===- PrototypeDebugContext.cpp - Signatures from a type library ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PrototypeDebugContext.h"

#include "neverd/Limits.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringExtras.h"

#include <limits>

namespace neverd::sdk {
namespace {

/// How deep a type converts: records hold records, which hold more.
constexpr unsigned MaxDepth = 16;

bool isDisplayName(llvm::StringRef Name) {
  return !Name.empty() &&
         (llvm::isAlpha(Name.front()) || Name.front() == '_') &&
         llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; });
}

class Converter {
public:
  explicit Converter(const types::TypeLibrary &Library) : Library(Library) {}

  TypeRef convert(const types::TypeExpr &Type, unsigned Depth) {
    if (Depth > MaxDepth)
      return nullptr;
    switch (Type.Kind) {
    case types::TypeExprKind::Builtin:
      return builtin(Type.TheBuiltin);
    case types::TypeExprKind::Pointer: {
      // A pointer's own value is all a decompiled signature needs: what it
      // points to only names it, so a pointee it cannot describe is void.
      TypeRef Pointee =
          Type.Element ? convert(*Type.Element, Depth + 1) : NdType::makeVoid();
      if (!Pointee)
        Pointee = NdType::makeVoid();
      TypeRef Pointer = NdType::makePtr(Pointee);
      Pointer->Size = uint16_t(Type.PointerBytes);
      return Pointer;
    }
    case types::TypeExprKind::Array: {
      TypeRef Element =
          Type.Element ? convert(*Type.Element, Depth + 1) : nullptr;
      const std::optional<uint64_t> Size = Library.sizeOf(Type);
      if (!Element || !Size || *Size > std::numeric_limits<uint16_t>::max())
        return nullptr;
      return NdType::makeArray(Element, uint32_t(Type.Count.value_or(0)),
                               uint16_t(*Size));
    }
    case types::TypeExprKind::Function:
      return Type.Function ? function(*Type.Function, Depth) : nullptr;
    case types::TypeExprKind::Record:
      return Type.Record ? record("", *Type.Record, Depth) : nullptr;
    case types::TypeExprKind::Enum:
      return Type.Enum ? builtin(Type.Enum->Underlying) : nullptr;
    case types::TypeExprKind::Named:
      return named(Type.Name, Depth);
    }
    return nullptr;
  }

private:
  TypeRef builtin(types::Builtin Builtin) {
    const types::BuiltinInfo &Info = types::builtinInfo(Builtin);
    const uint16_t Bytes =
        uint16_t(types::builtinBytes(Builtin, Library.dataModel()));
    switch (Builtin) {
    case types::Builtin::Void:
      return NdType::makeVoid();
    case types::Builtin::Half:
    case types::Builtin::Float:
    case types::Builtin::Double:
    case types::Builtin::LongDouble:
      return NdType::makeFloat(Bytes);
    default:
      break;
    }
    if (!Bytes)
      return nullptr;
    // The C spellings the decompiler keeps for the character types, as it
    // reads them from a PDB.
    TypeRef Integer = NdType::makeInt(Bytes, Info.Signed);
    if (Builtin == types::Builtin::Char)
      Integer->SourceName = "char";
    else if (Builtin == types::Builtin::SChar)
      Integer->SourceName = "signed char";
    else if (Builtin == types::Builtin::Bool)
      Integer->SourceName = "bool";
    else if (Builtin == types::Builtin::HResult)
      Integer->SourceName = "HRESULT";
    return Integer;
  }

  TypeRef named(llvm::StringRef Name, unsigned Depth) {
    const types::LocalType *Type = Library.find(Name);
    if (!Type)
      return nullptr;
    switch (Type->Kind) {
    case types::TypeKind::Typedef:
      return Type->Target ? convert(*Type->Target, Depth + 1) : nullptr;
    case types::TypeKind::Enum:
      return Type->Enum ? builtin(Type->Enum->Underlying) : nullptr;
    case types::TypeKind::Function:
      return Type->Function ? function(*Type->Function, Depth) : nullptr;
    case types::TypeKind::Struct:
    case types::TypeKind::Union:
      if (!Type->Record)
        return nullptr;
      if (auto Done = Records.find(Name); Done != Records.end())
        return Done->second;
      return record(Name, *Type->Record, Depth);
    }
    return nullptr;
  }

  /// A function type, as a pointer to it is spelled: scalars, and pointers
  /// opaque, as the C a callback's signature takes; void where a part of it
  /// is neither, or for a variadic one, whose calls pass more.
  TypeRef function(const types::FunctionDef &Function, unsigned Depth) {
    if (Function.Variadic)
      return NdType::makeVoid();
    const auto scalar = [&](const types::TypeExpr *Type) -> TypeRef {
      TypeRef Converted = Type ? convert(*Type, Depth + 1) : nullptr;
      if (!Converted)
        return nullptr;
      if (Converted->Kind == NdTypeKind::Ptr) {
        TypeRef Opaque = NdType::makePtr(NdType::makeVoid());
        Opaque->Size = Converted->Size;
        return Opaque;
      }
      return Converted->Kind == NdTypeKind::Int ||
                     Converted->Kind == NdTypeKind::Float
                 ? Converted
                 : nullptr;
    };
    TypeRef Return =
        Function.Return &&
                !(Function.Return->Kind == types::TypeExprKind::Builtin &&
                  Function.Return->TheBuiltin == types::Builtin::Void)
            ? scalar(Function.Return.get())
            : NdType::makeVoid();
    if (!Return)
      return NdType::makeVoid();
    std::vector<TypeRef> Params;
    for (const types::Parameter &Param : Function.Params) {
      TypeRef Converted = scalar(Param.Type.get());
      if (!Converted)
        return NdType::makeVoid();
      Params.push_back(std::move(Converted));
    }
    return NdType::makeFunc(std::move(Return), std::move(Params));
  }

  /// A record by its name, its members named for display where they lie, as
  /// a PDB's flat member list gives them: through records in place, which
  /// have no names of their own.  Bitfields share their storage's bytes and
  /// are left out.
  TypeRef record(llvm::StringRef Name, const types::RecordDef &Record,
                 unsigned Depth) {
    if (Record.Size > std::numeric_limits<uint16_t>::max())
      return nullptr;
    TypeRef Named = NdType::makeNamedRecord(Name.str(), uint16_t(Record.Size));
    // Local types are C's records, whose calls pass and return them as C
    // does: no constructor sends a small one through memory.
    Named->Passing = NdRecordPassing::ByValue;
    if (!Name.empty())
      Records[Name] = Named;
    if (Record.Defined)
      members(*Named, Record, 0, Depth);
    return Named;
  }

  void members(NdType &Into, const types::RecordDef &Record, uint64_t Base,
               unsigned Depth) {
    for (const types::Member &Field : Record.Members) {
      if (Into.FieldDisplayNames.size() >= limits::kMaxTpiDisplayFields)
        return;
      if (!Field.Type || Field.isBitfield())
        continue;
      const uint64_t Byte = (Base + Field.BitOffset) / 8;
      if (Byte > std::numeric_limits<uint16_t>::max())
        continue;
      if (Field.isAnonymous()) {
        members(Into, *Field.Type->Record, Base + Field.BitOffset, Depth);
        continue;
      }
      if (!isDisplayName(Field.Name))
        continue;
      Into.FieldDisplayOffsets.push_back(uint16_t(Byte));
      Into.FieldDisplayNames.push_back(Field.Name);
      Into.FieldDisplayTypes.push_back(convert(*Field.Type, Depth + 1));
    }
  }

  const types::TypeLibrary &Library;
  llvm::StringMap<TypeRef> Records;
};

} // namespace

TypeRef decompilerTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type) {
  return Converter(Library).convert(Type, 0);
}

llvm::StringSet<> prototypeTypeNames(const types::TypeLibrary &Library) {
  std::vector<std::string> Work;
  for (const auto &Entry : Library.prototypes()) {
    if (!Entry.second)
      continue;
    types::LocalType Signature;
    Signature.Kind = types::TypeKind::Function;
    Signature.Function = Entry.second;
    for (std::string &Name : types::mentionedTypeNames(Signature))
      Work.push_back(std::move(Name));
  }
  llvm::StringSet<> Names;
  while (!Work.empty()) {
    const std::string Name = std::move(Work.back());
    Work.pop_back();
    if (!Names.insert(Name).second)
      continue;
    if (const types::LocalType *Type = Library.find(Name))
      for (std::string &Named : types::mentionedTypeNames(*Type))
        Work.push_back(std::move(Named));
  }
  return Names;
}

PrototypeDebugContext::PrototypeDebugContext(
    const DebugContext &Inner, std::shared_ptr<const types::TypeLibrary> Types)
    : Inner(Inner), Types(std::move(Types)) {}

std::optional<FunctionSym>
PrototypeDebugContext::prototype(llvm::StringRef Name) const {
  std::lock_guard<std::mutex> Guard(Mutex);
  if (auto Known = Signatures.find(Name); Known != Signatures.end())
    return Known->second;
  std::optional<FunctionSym> &Signature = Signatures[Name];
  const types::FunctionDef *Function = Types->findPrototype(Name);
  if (!Function || Function->Variadic)
    return Signature;
  Converter Convert(*Types);
  FunctionSym Declared;
  Declared.Name = Name.str();
  Declared.ReturnType = Function->Return ? Convert.convert(*Function->Return, 0)
                                         : NdType::makeVoid();
  if (!Declared.ReturnType)
    return Signature;
  for (const types::Parameter &Param : Function->Params) {
    TypeRef Type = Param.Type ? Convert.convert(*Param.Type, 0) : nullptr;
    if (!Type || Type->Kind == NdTypeKind::Func ||
        Type->Kind == NdTypeKind::Void)
      return Signature;
    Declared.Params.emplace_back(Param.Name, std::move(Type));
  }
  Signature = std::move(Declared);
  return Signature;
}

std::optional<FunctionSym>
PrototypeDebugContext::resolveFunction(va_t Addr) const {
  std::optional<FunctionSym> Function = Inner.resolveFunction(Addr);
  // Only a function the debug information names at its entry and leaves
  // untyped: a declaration of the program's own describes its build.
  if (!Function || Function->Addr != Addr || Function->Name.empty() ||
      Function->ReturnType || !Function->Params.empty())
    return Function;
  if (std::optional<FunctionSym> Declared = prototype(Function->Name)) {
    Function->ReturnType = std::move(Declared->ReturnType);
    Function->Params = std::move(Declared->Params);
  }
  return Function;
}

} // namespace neverd::sdk
