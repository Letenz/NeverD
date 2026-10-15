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
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeRender.h"

#include "llvm/Support/JSON.h"

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

/// The session's local types, read from its PDB the first time; null with
/// the session's error when they cannot be read.
const types::TypeLibrary *localTypes(Session &S) {
  if (S.LocalTypes)
    return S.LocalTypes.get();
  if (S.DbgKind != DebugInfoKind::PDB || !S.Dbg) {
    S.LocalTypes = std::make_unique<types::TypeLibrary>(dataModelOf(S.Img));
    S.LocalTypesSource = "none";
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

} // extern "C"
