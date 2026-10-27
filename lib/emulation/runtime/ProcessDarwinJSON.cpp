//===- ProcessDarwinJSON.cpp - Explicit Darwin image inputs --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Strict module options shared by C, CLI and Python process requests.
#include "ProcessDarwinJSON.h"

#include "neverd/emulation/ProcessReportFields.h"
namespace neverd::emulation {
llvm::Expected<DarwinProcessOptions>
darwinOptionsFromJSON(const llvm::json::Value &Value) {
  namespace field = process_report;
  auto Invalid = [] {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "process: invalid darwin.modules");
  };
  const auto *Object = Value.getAsObject();
  if (!Object)
    return Invalid();
  DarwinProcessOptions Out;
  for (const auto &[Key, V] : *Object) {
    if (Key != field::Modules)
      return Invalid();
    const auto *Modules = V.getAsArray();
    if (!Modules || Modules->size() > DarwinModuleLimit)
      return Invalid();
    for (const auto &Item : *Modules) {
      const auto *Module = Item.getAsObject();
      if (!Module || Module->size() != 2)
        return Invalid();
      auto Name = Module->getString(field::Name);
      auto Path = Module->getString(field::Path);
      if (!Name || Name->empty() || Name->contains('\0') ||
          Name->size() > DarwinModuleNameLimit || !Path || Path->empty() ||
          Path->contains('\0'))
        return Invalid();
      Out.Modules.push_back(
          {Name->str(), std::filesystem::path(std::u8string(
                            reinterpret_cast<const char8_t *>(Path->data()),
                            Path->size()))});
    }
  }
  return Out;
}
} // namespace neverd::emulation
