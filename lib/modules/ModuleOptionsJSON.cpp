//===- ModuleOptionsJSON.cpp - Explicit module configuration -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit module configuration.
///

#include "neverd/modules/Limits.h"
#include "neverd/modules/ModuleOptions.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/JSON.h"

#include <array>
#include <fstream>
#include <set>

namespace neverd::modules {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "module configuration: " + Message);
}

llvm::Error keys(const llvm::json::Object &Object,
                 llvm::ArrayRef<llvm::StringRef> Allowed) {
  for (const auto &[Key, Value] : Object) {
    (void)Value;
    if (!llvm::is_contained(Allowed, llvm::StringRef(Key)))
      return invalid("unknown field: " + Key.str());
  }
  return llvm::Error::success();
}

llvm::Expected<LoadPolicy> policy(llvm::StringRef Text) {
  if (Text == "eager")
    return LoadPolicy::Eager;
  if (Text == "lazy")
    return LoadPolicy::Lazy;
  return invalid("load must be eager or lazy");
}

} // namespace

llvm::Expected<std::filesystem::path>
normalizeInputPath(llvm::StringRef Text, const std::filesystem::path &Base) {
  if (Text.empty() || Text.size() > input_limit::PathBytes ||
      Text.contains('\0') || !llvm::json::isUTF8(Text))
    return invalid("input paths must be nonempty bounded UTF-8");
  auto P = std::filesystem::path(std::u8string(Text.begin(), Text.end()));
  if (P.is_relative()) {
    if (Base.empty() || !Base.is_absolute())
      return invalid(
          "relative input requires an explicit absolute base_directory");
    P = Base / P;
  }
  return P.lexically_normal();
}

const char *loadPolicyName(LoadPolicy Policy) {
  switch (Policy) {
  case LoadPolicy::Eager:
    return "eager";
  case LoadPolicy::Lazy:
    return "lazy";
  }
  llvm_unreachable("invalid module load policy");
}

const char *debugKindName(DebugKind Kind) {
  switch (Kind) {
  case DebugKind::PDB:
    return "pdb";
  case DebugKind::DWARF:
    return "dwarf";
  case DebugKind::Map:
    return "map";
  }
  llvm_unreachable("invalid debug kind");
}

llvm::Expected<ModuleOptions> normalizeModuleOptions(ModuleOptions Options) {
  const auto &L = Options.Limits;
  if (!L.Modules ||
      L.Modules > ::neverd::limits::modules::kMaxModuleInstances ||
      !L.ImageBytes ||
      L.ImageBytes > ::neverd::limits::modules::kMaxModuleImageBytes ||
      !L.DebugBytes ||
      L.DebugBytes > ::neverd::limits::modules::kMaxModuleDebugBytes ||
      !L.TotalSourceBytes ||
      L.TotalSourceBytes >
          ::neverd::limits::modules::
              kModuleOptionsJSONNormalizeModuleOptionsLTotalSourceBytesLimit ||
      !L.Symbols || L.Symbols > ::neverd::limits::modules::kMaxModuleSymbols)
    return invalid("module limits must be positive and within their ceilings");
  if (Options.DefaultLoad != LoadPolicy::Eager &&
      Options.DefaultLoad != LoadPolicy::Lazy)
    return invalid("invalid default load policy");
  if (!Options.BaseDirectory.empty()) {
    auto Base = normalizeInputPath(pathToUTF8(Options.BaseDirectory));
    if (!Base)
      return Base.takeError();
    Options.BaseDirectory = std::move(*Base);
  }
  if (Options.Modules.size() > L.Modules)
    return invalid("module count exceeds its limit");
  std::set<std::string> IDs;
  for (auto &M : Options.Modules) {
    if (M.ID.empty() || M.ID.size() > input_limit::IDBytes ||
        !llvm::all_of(M.ID, [](char C) {
          return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
                 (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.';
        }))
      return invalid("id must contain 1..128 ASCII letters, digits, _, - or .");
    if (!IDs.insert(M.ID).second)
      return invalid("duplicate module id: " + M.ID);
    if (M.Load && *M.Load != LoadPolicy::Eager && *M.Load != LoadPolicy::Lazy)
      return invalid("invalid module load policy");
    auto Image = normalizeInputPath(pathToUTF8(M.Image), Options.BaseDirectory);
    if (!Image)
      return Image.takeError();
    M.Image = std::move(*Image);
    if (M.Debug) {
      if (M.Debug->Kind != DebugKind::PDB &&
          M.Debug->Kind != DebugKind::DWARF && M.Debug->Kind != DebugKind::Map)
        return invalid("invalid module debug kind");
      auto Debug =
          normalizeInputPath(pathToUTF8(M.Debug->Path), Options.BaseDirectory);
      if (!Debug)
        return Debug.takeError();
      M.Debug->Path = std::move(*Debug);
    }
  }
  return Options;
}

llvm::Expected<ModuleOptions>
moduleOptionsFromJSON(llvm::StringRef JSON,
                      const std::filesystem::path &BaseDirectory) {
  if (JSON.size() > input_limit::JSONBytes || JSON.contains('\0'))
    return invalid("JSON exceeds 2 MiB or contains a NUL");
  auto Parsed = llvm::json::parse(JSON);
  if (!Parsed)
    return Parsed.takeError();
  const auto *Root = Parsed->getAsObject();
  if (!Root)
    return invalid("expected an object");
  if (auto E = keys(*Root,
                    {"base_directory", "module_defaults", "limits", "modules"}))
    return std::move(E);
  ModuleOptions Options;
  Options.BaseDirectory = BaseDirectory;
  if (const auto *Value = Root->get("base_directory")) {
    auto Text = Value->getAsString();
    if (!Text)
      return invalid("base_directory must be a string");
    auto Base = normalizeInputPath(*Text);
    if (!Base)
      return Base.takeError();
    if (!BaseDirectory.empty() && BaseDirectory.lexically_normal() != *Base)
      return invalid("conflicting base_directory inputs");
    Options.BaseDirectory = std::move(*Base);
  }
  if (const auto *Value = Root->get("module_defaults")) {
    auto *Defaults = Value->getAsObject();
    if (!Defaults)
      return invalid("module_defaults must be an object");
    if (auto E = keys(*Defaults, {"load"}))
      return std::move(E);
    if (const auto *Load = Defaults->get("load")) {
      auto Text = Load->getAsString();
      if (!Text)
        return invalid("load must be a string");
      auto P = policy(*Text);
      if (!P)
        return P.takeError();
      Options.DefaultLoad = *P;
    }
  }
  if (const auto *Value = Root->get("limits")) {
    auto *Limits = Value->getAsObject();
    if (!Limits)
      return invalid("limits must be an object");
    if (auto E =
            keys(*Limits, {"max_modules", "max_image_bytes", "max_debug_bytes",
                           "max_total_source_bytes", "max_symbols"}))
      return std::move(E);
    for (auto [Name, Field] :
         {std::pair{"max_modules", &Options.Limits.Modules},
          {"max_image_bytes", &Options.Limits.ImageBytes},
          {"max_debug_bytes", &Options.Limits.DebugBytes},
          {"max_total_source_bytes", &Options.Limits.TotalSourceBytes},
          {"max_symbols", &Options.Limits.Symbols}}) {
      if (const auto *V = Limits->get(Name)) {
        auto N = V->getAsInteger();
        if (!N || *N <= 0)
          return invalid("limits must contain positive integers");
        *Field = uint64_t(*N);
      }
    }
  }
  if (const auto *Value = Root->get("modules")) {
    auto *Array = Value->getAsArray();
    if (!Array)
      return invalid("modules must be an array");
    if (Array->size() > Options.Limits.Modules ||
        Array->size() >
            ::neverd::limits::modules::
                kModuleOptionsJSONModuleOptionsFromJSONArraySizeLimit)
      return invalid("module count exceeds its limit");
    for (const auto &Item : *Array) {
      const auto *Object = Item.getAsObject();
      if (!Object)
        return invalid("each module must be an object");
      if (auto E = keys(*Object, {"id", "image", "debug", "load"}))
        return std::move(E);
      auto ID = Object->getString("id"), Image = Object->getString("image");
      if (!ID || !Image)
        return invalid("each module requires string id and image");
      ModuleInput Input;
      Input.ID = ID->str();
      Input.Image = std::u8string(Image->begin(), Image->end());
      if (const auto *Load = Object->get("load")) {
        auto Text = Load->getAsString();
        if (!Text)
          return invalid("load must be a string");
        auto P = policy(*Text);
        if (!P)
          return P.takeError();
        Input.Load = *P;
      }
      if (const auto *V = Object->get("debug")) {
        const auto *D = V->getAsObject();
        if (!D)
          return invalid("debug must be an object");
        if (auto E = keys(*D, {"kind", "path"}))
          return std::move(E);
        auto Kind = D->getString("kind"), Path = D->getString("path");
        if (!Kind || !Path)
          return invalid("debug requires string kind and path");
        DebugInput Debug;
        if (*Kind == "pdb")
          Debug.Kind = DebugKind::PDB;
        else if (*Kind == "dwarf")
          Debug.Kind = DebugKind::DWARF;
        else if (*Kind == "map")
          Debug.Kind = DebugKind::Map;
        else
          return invalid("debug kind must be pdb, dwarf or map");
        Debug.Path = std::u8string(Path->begin(), Path->end());
        Input.Debug = std::move(Debug);
      }
      Options.Modules.push_back(std::move(Input));
    }
  }
  return normalizeModuleOptions(std::move(Options));
}
llvm::Expected<ModuleOptions>
moduleOptionsFromFile(const std::filesystem::path &Configuration) {
  const auto TextPath = pathToUTF8(Configuration);
  if (TextPath.empty() || TextPath.size() > input_limit::PathBytes ||
      TextPath.find('\0') != std::string::npos || !llvm::json::isUTF8(TextPath))
    return invalid("configuration path must be bounded nonempty UTF-8");
  std::error_code EC;
  const auto Absolute =
      std::filesystem::absolute(Configuration, EC).lexically_normal();
  if (EC || !std::filesystem::is_regular_file(Absolute, EC) || EC)
    return invalid("configuration is not a readable regular file");
  std::ifstream Input(Absolute, std::ios::binary);
  if (!Input)
    return invalid("cannot open configuration file");
  std::string Text;
  std::array<char, 4096> Chunk;
  while (Input) {
    Input.read(Chunk.data(), Chunk.size());
    const auto N = static_cast<size_t>(Input.gcount());
    if (N > input_limit::JSONBytes - Text.size())
      return invalid("configuration JSON exceeds 2 MiB");
    Text.append(Chunk.data(), N);
  }
  if (!Input.eof())
    return invalid("cannot read complete configuration file");
  auto Root = llvm::json::parse(Text);
  if (!Root)
    return Root.takeError();
  auto Object = Root->getAsObject();
  if (!Object)
    return invalid("expected a configuration object");
  return moduleOptionsFromJSON(Text, Object->get("base_directory")
                                         ? std::filesystem::path{}
                                         : Absolute.parent_path());
}
} // namespace neverd::modules
