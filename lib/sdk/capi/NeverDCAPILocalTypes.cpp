//===- NeverDCAPILocalTypes.cpp - C API: local types ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The program's named types, read from its PDB the first time a frontend
/// asks, as a list, as one type laid out, and as source in a language.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPILocalTypes.h"

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/debug/PDBLoader.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectWriteLock.h"
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// The data model an image without types of its own lays types out in.
types::DataModel dataModelOf(const BinaryImage &Img) {
  if (Img.Bits == Bitness::Bits32)
    return types::DataModel::ILP32;
  return Img.Format == BinaryFormat::COFF ? types::DataModel::LLP64
                                          : types::DataModel::LP64;
}

/// Applies \p Declaration to \p Library: the types its source declares, or
/// the removal of those it names.
types::ParseResult apply(const Session::TypeDeclaration &Declaration,
                         types::TypeLibrary &Library) {
  types::ParseResult Result;
  if (!Declaration.Forget.empty()) {
    for (const std::string &Name : Declaration.Forget)
      if (!Library.remove(Name))
        Result.Problems.push_back({0, 0, "no type is named " + Name});
    return Result;
  }
  types::ParseOptions Options;
  Options.Lang = Declaration.Lang;
  Options.Rules = types::defaultRules(Library.dataModel());
  Options.Origin = types::TypeOrigin::User;
  return types::parseTypes(Declaration.Source, Library, Options);
}

/// The user's declarations, read again over freshly read types.  One the
/// program's types no longer admit, as those of another PDB may not, is
/// left out, and what it met is kept to report.
void redeclare(Session &S) {
  S.DeclarationProblems.clear();
  for (size_t I = 0; I < S.TypeDeclarations.size(); ++I)
    for (types::SourceProblem &Problem :
         apply(S.TypeDeclarations[I], *S.LocalTypes).Problems)
      S.DeclarationProblems.emplace_back(I, std::move(Problem));
}

std::filesystem::path declarationsPath(const Session &S) {
  auto Path = S.FilePath;
  Path += ".neverd-types.json";
  return Path;
}

llvm::json::Array declarationRows(const Session &S) {
  llvm::json::Array Rows;
  for (const Session::TypeDeclaration &Declaration : S.TypeDeclarations) {
    if (!Declaration.Forget.empty()) {
      llvm::json::Array Names;
      for (const std::string &Name : Declaration.Forget)
        Names.push_back(jsonSafeText(Name));
      Rows.push_back(llvm::json::Object{{"forget", std::move(Names)}});
      continue;
    }
    Rows.push_back(
        llvm::json::Object{{"language", types::languageId(Declaration.Lang)},
                           {"source", jsonSafeText(Declaration.Source)}});
  }
  return Rows;
}

/// The declarations \p Rows holds, or none with \p Error.
std::optional<std::vector<Session::TypeDeclaration>>
parseDeclarations(const llvm::json::Value &Rows, std::string &Error) {
  const llvm::json::Array *List = Rows.getAsArray();
  if (!List) {
    Error = "the type declarations are no list";
    return std::nullopt;
  }
  std::vector<Session::TypeDeclaration> Declarations;
  for (const llvm::json::Value &Row : *List) {
    const llvm::json::Object *Object = Row.getAsObject();
    Session::TypeDeclaration Declaration;
    if (Object && Object->get("forget")) {
      const llvm::json::Array *Names = Object->getArray("forget");
      if (!Names || Names->empty()) {
        Error = "a removal names no types";
        return std::nullopt;
      }
      for (const llvm::json::Value &Name : *Names) {
        std::optional<llvm::StringRef> Text = Name.getAsString();
        if (!Text || Text->empty()) {
          Error = "a removal names a type by no text";
          return std::nullopt;
        }
        Declaration.Forget.push_back(Text->str());
      }
      Declarations.push_back(std::move(Declaration));
      continue;
    }
    std::optional<llvm::StringRef> Language =
        Object ? Object->getString("language") : std::nullopt;
    std::optional<types::Language> Lang =
        Language ? types::languageNamed(*Language) : std::nullopt;
    std::optional<llvm::StringRef> Source =
        Object ? Object->getString("source") : std::nullopt;
    if (!Lang || !Source) {
      Error = "a type declaration needs a language (c, cpp, rust or go) and "
              "its source";
      return std::nullopt;
    }
    Declaration.Lang = *Lang;
    Declaration.Source = Source->str();
    Declarations.push_back(std::move(Declaration));
  }
  return Declarations;
}

/// The session's local types, read from its PDB the first time; null with
/// the session's error when they cannot be read.
types::TypeLibrary *localTypes(Session &S) {
  if (S.LocalTypes)
    return S.LocalTypes.get();
  if (S.DbgKind != DebugInfoKind::PDB || !S.Dbg) {
    S.LocalTypes = std::make_unique<types::TypeLibrary>(dataModelOf(S.Img));
    S.LocalTypesSource = "none";
    redeclare(S);
    return S.LocalTypes.get();
  }
  types::PDBTypeImportStats Stats;
  std::unique_ptr<types::TypeLibrary> Library;
  // The PDB the session holds open; a PDB 2.00 file it does not keep is
  // read again from its path.
  llvm::Error Err = static_cast<const PDBDebugContext &>(*S.Dbg).withPDBFile(
      [&](llvm::pdb::PDBFile &File) -> llvm::Error {
        auto Read = types::importPDBTypes(File, &Stats);
        if (!Read)
          return Read.takeError();
        Library = std::move(*Read);
        return llvm::Error::success();
      });
  if (Err && !Library) {
    llvm::consumeError(std::move(Err));
    auto Read = types::importPDBTypes(S.DbgPath.string(), &Stats);
    if (!Read) {
      S.setError("cannot read the PDB's types: " +
                 llvm::toString(Read.takeError()));
      return nullptr;
    }
    Library = std::move(*Read);
  } else if (Err) {
    S.setError("cannot read the PDB's types: " +
               llvm::toString(std::move(Err)));
    return nullptr;
  }
  S.LocalTypes = std::move(Library);
  S.LocalTypeStats = std::move(Stats);
  S.LocalTypesSource = "pdb";
  redeclare(S);
  return S.LocalTypes.get();
}

/// The options object, or null with the session's error.
std::optional<llvm::json::Object> options(Session &S, const char *Json) {
  if (!Json || !*Json)
    return llvm::json::Object();
  auto Parsed = llvm::json::parse(Json);
  if (!Parsed) {
    S.setError("malformed options: " + llvm::toString(Parsed.takeError()));
    return std::nullopt;
  }
  if (llvm::json::Object *Object = Parsed->getAsObject())
    return std::move(*Object);
  S.setError("options must be a JSON object");
  return std::nullopt;
}

std::optional<types::BuiltinSpelling> spellingNamed(llvm::StringRef Name) {
  if (Name == "windows")
    return types::BuiltinSpelling::Windows;
  if (Name == "ida")
    return types::BuiltinSpelling::IDA;
  if (Name == "keywords")
    return types::BuiltinSpelling::Keywords;
  if (Name == "portable")
    return types::BuiltinSpelling::Portable;
  return std::nullopt;
}

/// Reads "spelling" into \p Spelling; false with the session's error when it
/// names no spelling.
bool readSpelling(Session &S, const llvm::json::Object &Options,
                  types::BuiltinSpelling &Spelling) {
  const llvm::json::Value *Value = Options.get("spelling");
  if (!Value)
    return true;
  std::optional<llvm::StringRef> Name = Value->getAsString();
  std::optional<types::BuiltinSpelling> Found =
      Name ? spellingNamed(*Name) : std::nullopt;
  if (!Found) {
    S.setError("\"spelling\" must be windows, ida, keywords or portable");
    return false;
  }
  Spelling = *Found;
  return true;
}

/// Reads boolean \p Key into \p Out; false with the session's error when it
/// is no boolean.
bool readFlag(Session &S, const llvm::json::Object &Options,
              llvm::StringRef Key, bool &Out) {
  const llvm::json::Value *Value = Options.get(Key);
  if (!Value)
    return true;
  if (std::optional<bool> Flag = Value->getAsBoolean()) {
    Out = *Flag;
    return true;
  }
  S.setError(("\"" + Key + "\" must be true or false").str());
  return false;
}

} // namespace

extern "C" {

const char *neverd_local_types_json(neverd_session_t Sess,
                                    const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  std::string Filter;
  if (const llvm::json::Value *Value = Options->get("filter")) {
    std::optional<llvm::StringRef> Text = Value->getAsString();
    if (!Text) {
      S->setError("\"filter\" must be text");
      return nullptr;
    }
    Filter = Text->str();
  }
  std::optional<types::Language> Lang;
  if (const llvm::json::Value *Value = Options->get("language")) {
    std::optional<llvm::StringRef> Id = Value->getAsString();
    Lang = Id ? types::languageNamed(*Id) : std::nullopt;
    if (!Lang) {
      S->setError("\"language\" must be c, cpp, rust or go");
      return nullptr;
    }
  }
  const types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;

  llvm::json::Array Types;
  for (llvm::json::Value &Entry : types::typeListJSON(*Library)) {
    const llvm::json::Object *Object = Entry.getAsObject();
    if (!Filter.empty() && Object)
      if (std::optional<llvm::StringRef> Name = Object->getString("name"))
        if (!Name->contains_insensitive(Filter))
          continue;
    Types.push_back(std::move(Entry));
  }
  llvm::json::Array Mismatches;
  for (const std::string &Name : S->LocalTypeStats.NestingMismatches)
    Mismatches.push_back(jsonSafeText(Name));
  llvm::json::Object Result{
      {"source", S->LocalTypesSource},
      {"dataModel", types::dataModelName(Library->dataModel())},
      {"stats",
       llvm::json::Object{{"structs", S->LocalTypeStats.Structs},
                          {"unions", S->LocalTypeStats.Unions},
                          {"enums", S->LocalTypeStats.Enums},
                          {"declaredOnly", S->LocalTypeStats.DeclaredOnly},
                          {"nestingMismatches", std::move(Mismatches)}}},
      {"types", std::move(Types)}};
  if (!S->DeclarationProblems.empty()) {
    llvm::json::Array Problems;
    for (const auto &[Index, Problem] : S->DeclarationProblems)
      Problems.push_back(
          llvm::json::Object{{"declaration", static_cast<int64_t>(Index)},
                             {"line", Problem.Line},
                             {"column", Problem.Column},
                             {"message", jsonSafeText(Problem.Message)}});
    Result["declarationProblems"] = std::move(Problems);
  }
  if (Lang) {
    llvm::json::Array Renamed;
    for (const auto &[Name, Spelled] : types::renamedNames(*Library, *Lang))
      Renamed.push_back(
          llvm::json::Array{jsonSafeText(Name), jsonSafeText(Spelled)});
    Result["renamed"] = std::move(Renamed);
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_local_type_json(neverd_session_t Sess, const char *Name,
                                   const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (!Name) {
    S->setError("no type named");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  types::BuiltinSpelling Spelling = types::BuiltinSpelling::Windows;
  if (!readSpelling(*S, *Options, Spelling))
    return nullptr;
  const types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  const types::LocalType *Type = Library->find(Name);
  if (!Type) {
    S->setError(std::string("no local type named ") + jsonSafeText(Name));
    return nullptr;
  }
  return dupStr(jsonToString(
      llvm::json::Value(types::typeJSON(*Library, *Type, Spelling))));
}

const char *neverd_local_types_source(neverd_session_t Sess,
                                      const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  types::RenderOptions Render;
  if (const llvm::json::Value *Value = Options->get("language")) {
    std::optional<llvm::StringRef> Id = Value->getAsString();
    std::optional<types::Language> Lang =
        Id ? types::languageNamed(*Id) : std::nullopt;
    if (!Lang) {
      S->setError("\"language\" must be c, cpp, rust or go");
      return nullptr;
    }
    Render.Lang = *Lang;
  }
  bool Dependencies = true;
  if (!readSpelling(*S, *Options, Render.Spelling) ||
      !readFlag(*S, *Options, "offsets", Render.Offsets) ||
      !readFlag(*S, *Options, "hexOffsets", Render.HexOffsets) ||
      !readFlag(*S, *Options, "asserts", Render.Asserts) ||
      !readFlag(*S, *Options, "dependencies", Dependencies))
    return nullptr;
  const types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  std::vector<const types::LocalType *> Roots;
  if (const llvm::json::Value *Value = Options->get("types")) {
    const llvm::json::Array *Names = Value->getAsArray();
    if (!Names) {
      S->setError("\"types\" must be an array of names");
      return nullptr;
    }
    for (const llvm::json::Value &Entry : *Names) {
      std::optional<llvm::StringRef> Name = Entry.getAsString();
      const types::LocalType *Type = Name ? Library->find(*Name) : nullptr;
      if (!Type) {
        S->setError(Name ? "no local type named " + jsonSafeText(*Name)
                         : std::string("\"types\" must be an array of names"));
        return nullptr;
      }
      Roots.push_back(Type);
    }
  }
  if (!Roots.empty() && !Dependencies) {
    std::string Text;
    for (const types::LocalType *Type : Roots) {
      if (!Text.empty())
        Text += '\n';
      Text += types::renderType(*Library, *Type, Render);
    }
    return dupStr(Text);
  }
  return dupStr(types::renderTypes(*Library, Roots, Render));
}

const char *neverd_local_types_declare(neverd_session_t Sess,
                                       const char *Source,
                                       const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  Session::TypeDeclaration Declaration;
  if (const llvm::json::Value *Value = Options->get("language")) {
    std::optional<llvm::StringRef> Id = Value->getAsString();
    std::optional<types::Language> Lang =
        Id ? types::languageNamed(*Id) : std::nullopt;
    if (!Lang) {
      S->setError("\"language\" must be c, cpp, rust or go");
      return nullptr;
    }
    Declaration.Lang = *Lang;
  }
  types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  Declaration.Source = Source ? Source : "";
  const types::ParseResult Result = apply(Declaration, *Library);
  if (Result.ok())
    S->TypeDeclarations.push_back(std::move(Declaration));
  llvm::json::Array Types, Prototypes, Problems;
  for (const std::string &Name : Result.Types)
    Types.push_back(jsonSafeText(Name));
  for (const std::string &Name : Result.Prototypes)
    Prototypes.push_back(jsonSafeText(Name));
  for (const types::SourceProblem &Problem : Result.Problems)
    Problems.push_back(
        llvm::json::Object{{"line", Problem.Line},
                           {"column", Problem.Column},
                           {"message", jsonSafeText(Problem.Message)}});
  return dupStr(jsonToString(llvm::json::Value(
      llvm::json::Object{{"types", std::move(Types)},
                         {"prototypes", std::move(Prototypes)},
                         {"problems", std::move(Problems)}})));
}

const char *neverd_local_types_forget(neverd_session_t Sess,
                                      const char *NamesJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  llvm::Expected<llvm::json::Value> Parsed =
      llvm::json::parse(NamesJson ? NamesJson : "");
  if (!Parsed) {
    S->setError("the names are no JSON: " + llvm::toString(Parsed.takeError()));
    return nullptr;
  }
  std::string Error;
  std::optional<std::vector<Session::TypeDeclaration>> Removal =
      parseDeclarations(
          llvm::json::Array{llvm::json::Object{{"forget", std::move(*Parsed)}}},
          Error);
  if (!Removal) {
    S->setError(Error == "a removal names no types"
                    ? "name the types to remove in a list"
                    : Error);
    return nullptr;
  }
  types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  Session::TypeDeclaration &Declaration = Removal->front();
  for (const std::string &Name : Declaration.Forget)
    if (!Library->find(Name)) {
      S->setError("no type is named " + Name);
      return nullptr;
    }
  llvm::StringSet<> Removed;
  for (const std::string &Name : Declaration.Forget)
    Removed.insert(Name);
  apply(Declaration, *Library);
  // The types that still name one removed.
  llvm::json::Array UsedBy;
  for (const types::LocalType *Type : Library->types())
    for (const std::string &Named : types::mentionedTypeNames(*Type))
      if (Removed.contains(Named)) {
        UsedBy.push_back(jsonSafeText(Type->Name));
        break;
      }
  llvm::json::Array Names;
  for (const std::string &Name : Declaration.Forget)
    Names.push_back(jsonSafeText(Name));
  S->TypeDeclarations.push_back(std::move(Declaration));
  return dupStr(jsonToString(llvm::json::Value(llvm::json::Object{
      {"removed", std::move(Names)}, {"usedBy", std::move(UsedBy)}})));
}

const char *neverd_type_declarations_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  return dupStr(jsonToString(llvm::json::Value(declarationRows(*S))));
}

int neverd_type_declarations_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked() || !S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath);
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, declarationsPath(*S), declarationRows(*S),
                     "type declarations")
             ? 0
             : -1;
}

int neverd_type_declarations_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked() || !S->Loaded)
    return -1;
  const std::filesystem::path Path = declarationsPath(*S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect type declarations: " + EC.message());
    return -1;
  }
  std::vector<Session::TypeDeclaration> Declarations;
  if (Exists) {
    auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path));
    if (!Buffer) {
      S->setError("cannot read type declarations: " +
                  Buffer.getError().message());
      return -1;
    }
    llvm::Expected<llvm::json::Value> Parsed =
        llvm::json::parse((*Buffer)->getBuffer());
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("type declarations sidecar is not valid JSON");
      return -1;
    }
    std::string Error;
    std::optional<std::vector<Session::TypeDeclaration>> Read =
        parseDeclarations(*Parsed, Error);
    if (!Read) {
      S->setError("type declarations sidecar: " + Error);
      return -1;
    }
    Declarations = std::move(*Read);
  }
  S->TypeDeclarations = std::move(Declarations);
  S->dropLocalTypes();
  return 0;
}

} // extern "C"
