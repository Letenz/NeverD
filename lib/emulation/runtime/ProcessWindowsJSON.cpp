//===- ProcessWindowsJSON.cpp - Bounded explicit module inputs -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ProcessWindowsJSON.h"

#include "neverd/emulation/ProcessReportFields.h"
namespace neverd::emulation {
llvm::Expected<WindowsProcessOptions>
windowsOptionsFromJSON(const llvm::json::Value &Value) {
  namespace field = process_report;
  auto Invalid = [](llvm::StringRef Field) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   llvm::Twine(field::WindowsOptions) + Field);
  };
  const auto *Object = Value.getAsObject();
  if (!Object)
    return Invalid(field::Windows);
  WindowsProcessOptions Out;
  for (const auto &[Key, V] : *Object) {
    if (Key == field::PEBVersion) {
      const auto *Version = V.getAsObject();
      if (!Version || Version->size() != 4)
        return Invalid(Key);
      auto Major = Version->getInteger(field::VersionMajor);
      auto Minor = Version->getInteger(field::VersionMinor);
      auto Build = Version->getInteger(field::VersionBuild);
      auto Platform = Version->getInteger(field::VersionPlatform);
      if (!Major || *Major < 0 || uint64_t(*Major) > UINT32_MAX || !Minor ||
          *Minor < 0 || uint64_t(*Minor) > UINT32_MAX || !Build || *Build < 0 ||
          *Build > UINT16_MAX || !Platform || *Platform < 0 ||
          uint64_t(*Platform) > UINT32_MAX)
        return Invalid(Key);
      Out.PEBVersion = WindowsPEBVersion{uint32_t(*Major), uint32_t(*Minor),
                                         uint16_t(*Build), uint32_t(*Platform)};
      continue;
    }
    if (Key == field::DeferUnmodeled) {
      auto Defer = V.getAsBoolean();
      if (!Defer)
        return Invalid(Key);
      Out.DeferUnmodeled = *Defer;
      continue;
    }
    if (Key != field::Modules)
      return Invalid(Key);
    const auto *Modules = V.getAsArray();
    if (!Modules || Modules->size() > windows_process_limits::Modules)
      return Invalid(Key);
    for (const auto &Item : *Modules) {
      const auto *Module = Item.getAsObject();
      if (!Module || Module->size() != 2)
        return Invalid(Key);
      auto Name = Module->getString(field::Name);
      auto Path = Module->getString(field::Path);
      if (!Name || Name->empty() || Name->contains('\0') ||
          Name->size() >= windows_process_limits::NameBytes || !Path ||
          Path->empty() || Path->contains('\0'))
        return Invalid(Key);
      Out.Modules.push_back(
          {Name->str(), std::filesystem::path(std::u8string(
                            reinterpret_cast<const char8_t *>(Path->data()),
                            Path->size()))});
    }
  }
  return Out;
}
} // namespace neverd::emulation
