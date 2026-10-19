//===- NeverDCAPIModules.cpp - Shared module resource C boundary
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared module resource C boundary.
///

#include "neverd/sdk/NeverDCAPIModules.h"

#include "ModuleHandle.h"

#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

using namespace neverd;
namespace r = neverd::sdk::resources;
namespace {
llvm::Error failure(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
llvm::Expected<neverd_module_set_t> create(modules::ModuleOptions Options) {
  auto Base = Options.BaseDirectory;
  auto Sources = modules::ModuleSet::create(std::move(Options));
  if (!Sources)
    return Sources.takeError();
  auto State = std::make_shared<r::ModuleHandleState>();
  State->Sources = std::move(*Sources);
  State->BaseDirectory = std::move(Base);
  return r::retain(r::HandleKind::ModuleSet, std::move(State));
}
llvm::Expected<modules::ModuleInput>
inputJSON(const char *JSON, const std::filesystem::path &Base) {
  auto Text = r::text(JSON, NEVERD_MODULE_JSON_LIMIT, "module JSON");
  if (!Text)
    return Text.takeError();
  auto Value = llvm::json::parse(*Text);
  if (!Value)
    return Value.takeError();
  llvm::json::Array Inputs;
  Inputs.push_back(std::move(*Value));
  auto Options = modules::moduleOptionsFromJSON(
      llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                               {"modules", std::move(Inputs)}}))
          .str(),
      Base);
  if (!Options)
    return Options.takeError();
  return std::move(Options->Modules.front());
}
} // namespace
namespace neverd::sdk::resources {
llvm::Expected<std::shared_ptr<ModuleHandleState>> moduleState(uint64_t ID) {
  auto State = acquire(ID, HandleKind::ModuleSet);
  if (!State)
    return State.takeError();
  return std::static_pointer_cast<ModuleHandleState>(*State);
}
llvm::Expected<ModuleAccess> accessModule(uint64_t ID) {
  auto State = moduleState(ID);
  if (!State)
    return State.takeError();
  return accessModule(std::move(*State));
}
llvm::Expected<ModuleAccess>
accessModule(std::shared_ptr<ModuleHandleState> State) {
  if (!State || State->Busy.exchange(true))
    return failure("module resources are busy in another operation");
  return ModuleAccess(std::move(State));
}
llvm::Error addModuleJSON(ModuleHandleState &State, const char *JSON) {
  auto Input = inputJSON(JSON, State.BaseDirectory);
  if (!Input)
    return Input.takeError();
  return State.Sources->add(std::move(*Input));
}
llvm::Error reviseModuleJSON(ModuleHandleState &State, const char *JSON) {
  auto Input = inputJSON(JSON, State.BaseDirectory);
  if (!Input)
    return Input.takeError();
  if (Input->Load)
    return failure(
        "unprepared revision must omit load and retain its original policy");
  return State.Sources->reviseUnprepared(Input->ID, Input->Image,
                                         std::move(Input->Debug));
}
llvm::Error attachModuleDebugJSON(ModuleHandleState &State, const char *ID,
                                  const char *JSON) {
  auto Name = r::text(ID, NEVERD_MODULE_ID_LIMIT, "module id");
  if (!Name)
    return Name.takeError();
  auto DebugText = r::text(JSON, NEVERD_MODULE_JSON_LIMIT, "debug JSON");
  if (!DebugText)
    return DebugText.takeError();
  auto Debug = llvm::json::parse(*DebugText);
  if (!Debug)
    return Debug.takeError();
  for (const auto &M : State.Sources->inspect()) {
    if (M.Input.ID != *Name)
      continue;
    auto Input =
        inputJSON(llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                           {"id", Name->str()},
                                           {"image", pathToUTF8(M.Input.Image)},
                                           {"debug", std::move(*Debug)}}))
                      .str()
                      .c_str(),
                  State.BaseDirectory);
    if (!Input)
      return Input.takeError();
    auto Image = State.Sources->attachDebug(*Name, std::move(*Input->Debug));
    if (!Image)
      return Image.takeError();
    return llvm::Error::success();
  }
  return failure("unknown module id");
}
} // namespace neverd::sdk::resources

extern "C" neverd_module_set_t
neverd_module_set_create_v1(const neverd_module_set_options_v1 *Options,
                            const char **Error) {
  return r::boundary<neverd_module_set_t>(
      Error, 0, [&]() -> llvm::Expected<neverd_module_set_t> {
        if (Options && Options->struct_size != sizeof(*Options))
          return failure("module set options require exact v1 struct_size");
        auto Text = r::text(
            Options && Options->configuration_json ? Options->configuration_json
                                                   : "{}",
            NEVERD_MODULE_JSON_LIMIT, "module configuration JSON");
        if (!Text)
          return Text.takeError();
        std::filesystem::path Base;
        if (Options && Options->base_directory) {
          auto Path = r::text(Options->base_directory, NEVERD_MODULE_PATH_LIMIT,
                              "module base directory");
          if (!Path)
            return Path.takeError();
          Base = std::u8string(Path->begin(), Path->end());
        }
        auto Parsed = modules::moduleOptionsFromJSON(*Text, Base);
        if (!Parsed)
          return Parsed.takeError();
        return create(std::move(*Parsed));
      });
}
extern "C" neverd_module_set_t neverd_module_set_from_file(const char *Path,
                                                           const char **Error) {
  return r::boundary<neverd_module_set_t>(
      Error, 0, [&]() -> llvm::Expected<neverd_module_set_t> {
        auto Input = r::text(Path, NEVERD_MODULE_PATH_LIMIT,
                             "module configuration path");
        if (!Input)
          return Input.takeError();
        auto Options = modules::moduleOptionsFromFile(
            std::u8string(Input->begin(), Input->end()));
        if (!Options)
          return Options.takeError();
        return create(std::move(*Options));
      });
}
extern "C" int neverd_module_set_destroy(neverd_module_set_t Set,
                                         const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    if (Set)
      if (auto E = r::release(Set, r::HandleKind::ModuleSet))
        return std::move(E);
    return 1;
  });
}
extern "C" const char *neverd_module_set_inspect_json(neverd_module_set_t Set,
                                                      const char **Error) {
  return r::boundary<const char *>(
      Error, nullptr, [&]() -> llvm::Expected<const char *> {
        auto Access = r::accessModule(Set);
        if (!Access)
          return Access.takeError();
        return r::ownedString(modules::moduleSetJSON(*Access->state().Sources));
      });
}
extern "C" int neverd_module_set_add_json(neverd_module_set_t Set,
                                          const char *JSON,
                                          const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    auto Access = r::accessModule(Set);
    if (!Access)
      return Access.takeError();
    if (auto E = r::addModuleJSON(Access->state(), JSON))
      return std::move(E);
    return 1;
  });
}
extern "C" int neverd_module_set_revise_unprepared_json(neverd_module_set_t Set,
                                                        const char *JSON,
                                                        const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    auto Access = r::accessModule(Set);
    if (!Access)
      return Access.takeError();
    if (auto E = r::reviseModuleJSON(Access->state(), JSON))
      return std::move(E);
    return 1;
  });
}
extern "C" int neverd_module_set_prepare(neverd_module_set_t Set,
                                         const char *ID, const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    auto Name = r::text(ID, NEVERD_MODULE_ID_LIMIT, "module id");
    if (!Name)
      return Name.takeError();
    auto Access = r::accessModule(Set);
    if (!Access)
      return Access.takeError();
    auto Image = Access->state().Sources->prepare(*Name);
    if (!Image)
      return Image.takeError();
    return 1;
  });
}
extern "C" int neverd_module_set_prepare_eager(neverd_module_set_t Set,
                                               const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    auto Access = r::accessModule(Set);
    if (!Access)
      return Access.takeError();
    if (auto E = Access->state().Sources->prepareEager())
      return std::move(E);
    return 1;
  });
}
extern "C" int neverd_module_set_attach_debug_json(neverd_module_set_t Set,
                                                   const char *ID,
                                                   const char *JSON,
                                                   const char **Error) {
  return r::boundary<int>(Error, 0, [&]() -> llvm::Expected<int> {
    auto Access = r::accessModule(Set);
    if (!Access)
      return Access.takeError();
    if (auto E = r::attachModuleDebugJSON(Access->state(), ID, JSON))
      return std::move(E);
    return 1;
  });
}
extern "C" const char *
neverd_module_set_lookup_symbol_json(neverd_module_set_t Set, const char *ID,
                                     const char *Name, const char **Error) {
  return r::boundary<const char *>(
      Error, nullptr, [&]() -> llvm::Expected<const char *> {
        auto Module = r::text(ID, NEVERD_MODULE_ID_LIMIT, "module id");
        if (!Module)
          return Module.takeError();
        auto Symbol = r::text(Name, NEVERD_MODULE_SYMBOL_LIMIT, "symbol name");
        if (!Symbol)
          return Symbol.takeError();
        auto Access = r::accessModule(Set);
        if (!Access)
          return Access.takeError();
        auto Found = Access->state().Sources->lookupSymbol(*Module, *Symbol);
        if (!Found)
          return Found.takeError();
        llvm::json::Object Result{{"module_id", Module->str()},
                                  {"found", Found->has_value()}};
        if (*Found) {
          const auto &S = **Found;
          Result["symbol"] = llvm::json::Object{
              {"name", S.Name},
              {"address", "0x" + llvm::utohexstr(S.Address)},
              {"size", "0x" + llvm::utohexstr(S.Size)},
              {"kind",
               S.Kind == modules::SymbolKind::Function ? "function" : "data"}};
        }
        return r::ownedString(
            llvm::formatv("{0:2}", llvm::json::Value(std::move(Result))).str());
      });
}
