//===- LocalTypesDebugContext.cpp - Debug info from local types -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LocalTypesDebugContext.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringExtras.h"

#include <limits>
#include <set>

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

/// Whether every member \p Record names for display is one \p Def has
/// where the name says, and is no bitfield, which C reads otherwise than
/// its storage's bytes.
bool membersAgree(const NdType &Record, const types::RecordDef &Def) {
  if (Record.FieldDisplayNames.size() != Record.FieldDisplayOffsets.size())
    return Record.FieldDisplayNames.empty();
  std::set<std::pair<uint64_t, std::string>> Members;
  for (const types::FlatMember &Flat : types::flattenMembers(Def))
    if (!Flat.BitWidth && !Flat.Name.empty())
      Members.emplace(Flat.BitOffset / 8, Flat.Name);
  for (size_t I = 0; I < Record.FieldDisplayNames.size(); ++I)
    if (!Record.FieldDisplayNames[I].empty() &&
        !Members.count(
            {Record.FieldDisplayOffsets[I], Record.FieldDisplayNames[I]}))
      return false;
  return true;
}

} // namespace

TypeRef decompilerTypeOf(const types::TypeLibrary &Library,
                         const types::TypeExpr &Type) {
  return Converter(Library).convert(Type, 0);
}

std::optional<FunctionSym> signatureOf(const types::TypeLibrary &Library,
                                       const types::FunctionDef &Function,
                                       llvm::StringRef Name) {
  if (Function.Variadic)
    return std::nullopt;
  Converter Convert(Library);
  FunctionSym Declared;
  Declared.Name = Name.str();
  Declared.NoReturn = Function.NoReturn;
  Declared.ReturnType = Function.Return ? Convert.convert(*Function.Return, 0)
                                        : NdType::makeVoid();
  if (!Declared.ReturnType)
    return std::nullopt;
  for (const types::Parameter &Param : Function.Params) {
    TypeRef Type = Param.Type ? Convert.convert(*Param.Type, 0) : nullptr;
    if (!Type || Type->Kind == NdTypeKind::Func ||
        Type->Kind == NdTypeKind::Void)
      return std::nullopt;
    Declared.Params.emplace_back(Param.Name, std::move(Type));
  }
  switch (Function.Convention) {
  case types::CallingConvention::Cdecl:
    Declared.CallConv = DebugCallConv::Cdecl;
    break;
  case types::CallingConvention::Stdcall:
    Declared.CallConv = DebugCallConv::Stdcall;
    break;
  case types::CallingConvention::Fastcall:
    Declared.CallConv = DebugCallConv::Fastcall;
    break;
  case types::CallingConvention::Thiscall:
    Declared.CallConv = DebugCallConv::Thiscall;
    break;
  default:
    break;
  }
  return Declared;
}

namespace {

/// How a value of \p Type passes: void, a float, a record or an integer or
/// pointer, with its size.
std::pair<NdTypeKind, uint16_t> passing(const TypeRef &Type) {
  if (!Type || Type->Kind == NdTypeKind::Void)
    return {NdTypeKind::Void, 0};
  if (Type->Kind == NdTypeKind::Float ||
      (Type->Kind == NdTypeKind::Struct && !Type->IsEnum))
    return {Type->Kind, Type->Size};
  return {NdTypeKind::Int, Type->Size};
}

} // namespace

bool passesAlike(const FunctionSym &A, const FunctionSym &B) {
  if (A.Params.size() != B.Params.size() ||
      passing(A.ReturnType) != passing(B.ReturnType))
    return false;
  for (size_t I = 0; I < A.Params.size(); ++I)
    if (passing(A.Params[I].second) != passing(B.Params[I].second))
      return false;
  return true;
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

LocalTypesDebugContext::LocalTypesDebugContext(
    const DebugContext &Inner, std::shared_ptr<const types::TypeLibrary> Types,
    std::map<va_t, std::string> Declarations, const BinaryImage *Image,
    std::vector<types::HeaderLibrary> Headers,
    std::unordered_map<uint64_t, std::string> Recognized)
    : Inner(Inner), Types(std::move(Types)),
      Declarations(std::move(Declarations)), Image(Image),
      Headers(std::move(Headers)), Recognized(std::move(Recognized)) {}

LocalTypesDebugContext::~LocalTypesDebugContext() {
  std::lock_guard<std::mutex> Guard(Mutex);
  for (const TypeRef &Record : Completed) {
    Record->FieldDisplayNames.clear();
    Record->FieldDisplayOffsets.clear();
    Record->FieldDisplayTypes.clear();
  }
}

const types::LocalType *
LocalTypesDebugContext::definedRecord(llvm::StringRef Name,
                                      uint16_t Size) const {
  const types::LocalType *Type = Types->find(Name);
  if (!Type || !Type->Record || !Type->Record->Defined ||
      (Type->Kind != types::TypeKind::Struct &&
       Type->Kind != types::TypeKind::Union))
    return nullptr;
  return !Size || Size == Type->Record->Size ? Type : nullptr;
}

void LocalTypesDebugContext::completeType(const TypeRef &Type) const {
  TypeRef Record = Type;
  while (Record && Record->Kind == NdTypeKind::Ptr)
    Record = Record->Pointee;
  if (Record && Record->Kind == NdTypeKind::Struct && !Record->IsEnum &&
      !Record->SourceName.empty()) {
    std::lock_guard<std::mutex> Guard(Mutex);
    if (CompletedRecords.contains(Record.get()))
      return;
    // The library's members, which a record none has been given members
    // takes whole: the debug information's own stay where it gave them.
    if (const types::LocalType *Local =
            definedRecord(Record->SourceName, Record->Size);
        Local && Record->FieldDisplayNames.empty()) {
      const TypeRef From = Converter(*Types).convert(
          *types::makeNamed(Local->Name, Local->Kind), 0);
      if (From && From->Kind == NdTypeKind::Struct) {
        Record->FieldDisplayNames = From->FieldDisplayNames;
        Record->FieldDisplayOffsets = From->FieldDisplayOffsets;
        Record->FieldDisplayTypes = From->FieldDisplayTypes;
        Completed.push_back(Record);
        CompletedRecords.insert(Record.get());
        Read.insert(Local->Name);
        return;
      }
    }
  }
  Inner.completeType(Type);
  // What the debug information gave a record or an enumeration of one of
  // the library's names: a unit holds the library's definition only where
  // that agrees with it.
  if (Record && Record->Kind == NdTypeKind::Struct &&
      !Record->SourceName.empty() &&
      (Record->IsEnum || !Record->FieldDisplayNames.empty())) {
    std::lock_guard<std::mutex> Guard(Mutex);
    const types::LocalType *Local = Types->find(Record->SourceName);
    if (Local && !CompletedRecords.contains(Record.get()) &&
        (Record->IsEnum
             ? Local->Kind == types::TypeKind::Enum
             : definedRecord(Record->SourceName, Record->Size) != nullptr) &&
        GivenTypes.insert(Record.get()).second)
      Given[Record->SourceName].push_back(Record);
  }
}

CRecordDefinitions
LocalTypesDebugContext::definitions(const std::vector<TypeRef> &Records) const {
  std::lock_guard<std::mutex> Guard(Mutex);
  if (!RenamedInC) {
    RenamedInC.emplace();
    for (const auto &[Name, Spelled] :
         types::renamedNames(*Types, types::Language::C)) {
      RenamedInC->insert(Name);
      // An enumerator's key is its enumeration's: Owner::Value.
      if (const size_t Scope = Name.find("::"); Scope != std::string::npos)
        RenamedInC->insert(Name.substr(0, Scope) + "::");
    }
  }
  // Whether \p Type, of the library's type \p Local, names its members or
  // enumerators as the library does.
  const auto Agrees = [&](const NdType &Type, const types::LocalType &Local) {
    if (Local.Kind != types::TypeKind::Enum)
      return Local.Record && membersAgree(Type, *Local.Record);
    // An enumeration none of whose enumerators C spells otherwise.
    return Local.Enum && !RenamedInC->contains(Local.Name + "::") &&
           llvm::all_of(Type.FieldDisplayNames, [&](const std::string &Name) {
             return Name.empty() ||
                    llvm::any_of(Local.Enum->Values,
                                 [&](const types::Enumerator &Value) {
                                   return Value.Name == Name;
                                 });
           });
  };
  // ... as each type the debug information gave of its name does.
  const auto AllAgree = [&](const NdType &Type, const types::LocalType &Local) {
    const auto Types = Given.find(Local.Name);
    return Agrees(Type, Local) &&
           (Types == Given.end() ||
            llvm::all_of(Types->second, [&](const TypeRef &Other) {
              return Other->IsEnum == (Local.Kind == types::TypeKind::Enum) &&
                     Agrees(*Other, Local);
            }));
  };
  // The enumerators of the enumerations the decompiler displays, which a
  // unit can spell without their enumeration's name.
  llvm::StringMap<const types::LocalType *> Enumerators;
  for (const auto &Entry : Given)
    for (const TypeRef &Enum : Entry.getValue())
      if (const types::LocalType *Local = Types->find(Entry.getKey());
          Enum->IsEnum && Local && Local->Kind == types::TypeKind::Enum &&
          AllAgree(*Enum, *Local))
        for (const std::string &Name : Enum->FieldDisplayNames)
          if (!Name.empty())
            Enumerators.try_emplace(Name, Local);
  CRecordDefinitions Result;
  std::vector<const types::LocalType *> Roots;
  for (const TypeRef &Record : Records) {
    if (!Record || Record->Kind != NdTypeKind::Struct ||
        Record->SourceName.empty() || RenamedInC->contains(Record->SourceName))
      continue;
    const types::LocalType *Local = Types->find(Record->SourceName);
    // A name the unit only spells.
    const bool Spelled =
        !Record->IsEnum && !Record->Size && Record->FieldDisplayNames.empty();
    if (!Local) {
      if (const auto Owner = Enumerators.find(Record->SourceName);
          Spelled && Owner != Enumerators.end())
        Roots.push_back(Owner->second);
      continue;
    }
    if (Local->Kind == types::TypeKind::Struct ||
        Local->Kind == types::TypeKind::Union)
      Result.Tags[Record->SourceName] = Local->Kind == types::TypeKind::Union;
    if (Record->IsEnum || (Spelled && Local->Kind == types::TypeKind::Enum)) {
      if (Local->Kind == types::TypeKind::Enum && AllAgree(*Record, *Local))
        Roots.push_back(Local);
      continue;
    }
    // A record the unit names members of otherwise than the library does
    // stays declared only: its definition would disagree with them.
    if (const types::LocalType *Defined =
            definedRecord(Record->SourceName, Record->Size);
        Defined && AllAgree(*Record, *Defined))
      Roots.push_back(Defined);
  }
  if (Roots.empty())
    return Result;
  types::RenderOptions Options;
  Options.Lang = types::Language::C;
  // Exact widths, which mean the same on every compiler the unit builds
  // with; the checks make a layout no compiler agrees with an error.
  Options.Spelling = types::BuiltinSpelling::Portable;
  Options.Asserts = true;
  const types::EmbeddedC Embedded =
      types::renderEmbeddedC(*Types, Roots, Options);
  Result.Text = Embedded.Text;
  for (const auto &[Name, Union] : Embedded.Records)
    Result.Records[Name] = Result.Tags[Name] = Union;
  for (const auto &[Name, Union] : Embedded.Declared)
    Result.Tags[Name] = Union;
  Result.Identifiers = Embedded.Identifiers;
  for (const std::string &Name : Embedded.Types) {
    Read.insert(Name);
    if (const types::LocalType *Type = Types->find(Name);
        Type && Type->Kind == types::TypeKind::Enum)
      Result.Enumerations.push_back(Name);
  }
  Result.Types = Embedded.Types;
  Result.DeclaredTypes = Embedded.DeclaredTypes;
  return Result;
}

llvm::StringSet<> LocalTypesDebugContext::readTypes() const {
  std::lock_guard<std::mutex> Guard(Mutex);
  llvm::StringSet<> Names;
  for (const auto &Name : Read)
    Names.insert(Name.getKey());
  return Names;
}

std::optional<types::HeaderLibrary> LocalTypesDebugContext::prototypeHeader(
    llvm::StringRef Name, const types::FunctionDef *Function) const {
  // Joining a header library shares its prototypes.
  for (const types::HeaderLibrary Library : Headers)
    if (const types::TypeLibrary *Header = types::headerLibrary(Library))
      if (Header->findPrototype(Name) == Function)
        return Library;
  return std::nullopt;
}

std::optional<FunctionSym>
LocalTypesDebugContext::resolvePrototype(llvm::StringRef Name) const {
  std::lock_guard<std::mutex> Guard(Mutex);
  if (auto Known = Signatures.find(Name); Known != Signatures.end())
    return Known->second;
  std::optional<FunctionSym> &Signature = Signatures[Name];
  const auto Function = Types->prototypes().find(Name);
  if (Function == Types->prototypes().end() || !Function->second)
    return Signature;
  Signature = signatureOf(*Types, *Function->second, Name);
  if (!Signature)
    return Signature;
  const std::optional<types::HeaderLibrary> Header =
      prototypeHeader(Name, Function->second.get());
  Signature->PrototypeLibrary =
      Header ? types::headerLibraryName(*Header).str() : "local-types";
  types::RenderOptions Options;
  Options.Lang = types::Language::C;
  if (Header)
    Options.Spelling = types::headerLibrarySpelling(*Header);
  std::unique_ptr<types::TypeSpeller> &Speller =
      Spellers[types::spellingOf(Options)];
  if (!Speller)
    Speller = std::make_unique<types::TypeSpeller>(*Types, Options);
  Signature->PrototypeDeclaration =
      (Function->second->NoReturn ? "_Noreturn " : "") +
      Speller->spell(*types::makeFunction(Function->second), Name.str());
  return Signature;
}

std::vector<std::string> LocalTypesDebugContext::statedNames(va_t Entry) const {
  if (!Image)
    return {};
  std::lock_guard<std::mutex> Guard(Mutex);
  auto [Known, Added] = StatedNames.try_emplace(Entry);
  if (Added)
    for (const llvm::StringRef Name : Image->statedFunctionNamesAt(Entry))
      Known->second.push_back(Name.str());
  return Known->second;
}

std::string LocalTypesDebugContext::importName(va_t Addr) const {
  if (!Image)
    return {};
  std::lock_guard<std::mutex> Guard(Mutex);
  auto [Known, Added] = ImportNames.try_emplace(Addr);
  if (Added)
    Known->second = importCalleeName(*Image, Addr);
  return Known->second;
}

std::optional<FunctionSym> LocalTypesDebugContext::declared(va_t Entry) const {
  const auto Declaration = Declarations.find(Entry);
  if (Declaration == Declarations.end())
    return std::nullopt;
  std::lock_guard<std::mutex> Guard(Mutex);
  if (auto Known = DeclaredSignatures.find(Entry);
      Known != DeclaredSignatures.end())
    return Known->second;
  std::optional<FunctionSym> &Signature = DeclaredSignatures[Entry];
  const types::FunctionDeclaration Read =
      types::parseFunctionDeclaration(Declaration->second, *Types);
  if (Read.ok())
    Signature = signatureOf(*Types, *Read.Function, Read.Name);
  return Signature;
}

std::optional<FunctionSym>
LocalTypesDebugContext::resolveFunction(va_t Addr) const {
  std::optional<FunctionSym> Function = Inner.resolveFunction(Addr);
  const bool AtEntry = Function && Function->Addr == Addr;
  // The user's declaration states the function's parameters and return,
  // whatever the debug information says; its name and extent stay.
  if (std::optional<FunctionSym> User = declared(Addr)) {
    FunctionSym Declared = AtEntry ? *Function : FunctionSym();
    Declared.Addr = Addr;
    Declared.ReturnType = std::move(User->ReturnType);
    Declared.Params = std::move(User->Params);
    if (User->CallConv != DebugCallConv::Unknown)
      Declared.CallConv = User->CallConv;
    Declared.PlatformConvention = true;
    return Declared;
  }
  // A declaration of the program's own describes its build.
  if (AtEntry && (Function->ReturnType || !Function->Params.empty()))
    return Function;
  // The prototype of the name the function is displayed under: the export
  // or symbol the image states, else the debug information's name.  Where a
  // linker folded identical functions, the entry's other names name other
  // functions, whose prototypes must pass and return alike for the code to
  // follow the shown one.
  const std::vector<std::string> Names = statedNames(Addr);
  std::string Name = Names.empty() ? std::string() : Names.front();
  if (Name.empty() && AtEntry)
    Name = Function->Name;
  // Signatures name a function by its bytes where nothing else does.
  if (const auto Matched = Recognized.find(Addr);
      Name.empty() && Matched != Recognized.end())
    Name = Matched->second;
  std::optional<FunctionSym> Known =
      Name.empty() ? std::nullopt : resolvePrototype(Name);
  if (Known)
    for (size_t I = 1; I < Names.size(); ++I)
      if (const std::optional<FunctionSym> Other = resolvePrototype(Names[I]);
          Other && !passesAlike(*Known, *Other))
        return Function;
  // A call through an import's slot or stub reaches the routine another
  // module exports under the import's name, which the call names it by.
  if (!Known && !AtEntry)
    if (const std::string Imported = importName(Addr); !Imported.empty())
      if ((Known = resolvePrototype(Imported))) {
        Known->Addr = Addr;
        return Known;
      }
  if (!Known)
    return Function;
  FunctionSym Typed = AtEntry ? *Function : FunctionSym();
  Typed.Addr = Addr;
  Typed.ReturnType = std::move(Known->ReturnType);
  Typed.Params = std::move(Known->Params);
  if (Known->CallConv != DebugCallConv::Unknown)
    Typed.CallConv = Known->CallConv;
  Typed.PlatformConvention = true;
  Typed.PrototypeLibrary = std::move(Known->PrototypeLibrary);
  Typed.PrototypeDeclaration = std::move(Known->PrototypeDeclaration);
  Typed.NoReturn = Known->NoReturn;
  return Typed;
}

} // namespace neverd::sdk
