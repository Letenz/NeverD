#include "DarwinSourceDeclarations.h"

#include "DarwinRuntimeImport.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include <algorithm>
#include <map>

namespace neverd {
namespace {
struct Declaration {
  const char *Name;
  const char *AArch64;
  const char *X64;
  const char *AArch64Modules;
  const char *X64Modules;
};
constexpr Declaration Declarations[] = {
#include "DarwinSourceDeclarations.inc"
};

llvm::StringRef canonicalModule(llvm::StringRef Module) {
  // These frameworks use unversioned install names on iOS. All other names
  // retain their exact identity, including private/user framework paths.
  if (Module == "/System/Library/Frameworks/Foundation.framework/Foundation")
    return "/System/Library/Frameworks/Foundation.framework/Versions/C/"
           "Foundation";
  if (Module ==
      "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    return "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/"
           "CoreFoundation";
  return Module;
}

bool exportsFrom(llvm::StringRef Modules, llvm::StringRef Module) {
  if (Module.empty())
    return false;
  Module = canonicalModule(Module);
  while (!Modules.empty()) {
    const auto [Current, Remaining] = Modules.split('|');
    if (Current == Module)
      return true;
    Modules = Remaining;
  }
  return false;
}

using Index =
    std::map<std::string, std::optional<SourceFunctionTypeHint>, std::less<>>;
Index signatures(Arch Architecture) {
  Index Result;
  for (const auto &D : Declarations) {
    const char *Encoding = Architecture == Arch::AArch64 ? D.AArch64 : D.X64;
    auto Hint = Encoding ? parseObjCFunctionEncoding(Encoding) : std::nullopt;
    std::string Diagnostic;
    if (Hint) {
      Hint->Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
      if (!assignDarwinScalarSourceABI(*Hint, Architecture, Diagnostic))
        Hint.reset();
    }
    auto [It, Added] = Result.try_emplace(D.Name, Hint);
    // C functions have one complete declaration per linker identity. An
    // alternative declaration is not resolved by visitation order.
    if (!Added)
      It->second.reset();
  }
  return Result;
}
} // namespace

std::optional<SourceCallTypeHint>
darwinDeclaredSourceCallHint(const BinaryImage &Image, va_t ImportSlot) {
  auto Import = darwinRuntimeImport(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Import || !Import->consume_front("_") ||
      Bind == Image.DyldBindSlots.end() ||
      libc::isReturnsTwiceFunction(Import->str()))
    return std::nullopt;
  const auto D = std::lower_bound(
      std::begin(Declarations), std::end(Declarations), *Import,
      [](const Declaration &D, llvm::StringRef Name) { return D.Name < Name; });
  if (D == std::end(Declarations) || D->Name != *Import ||
      !exportsFrom(Image.Arch == Arch::AArch64 ? D->AArch64Modules
                                               : D->X64Modules,
                   Bind->second.Module))
    return std::nullopt;
  static const auto Arm = signatures(Arch::AArch64);
  static const auto Intel = signatures(Arch::X64);
  const auto &Index = Image.Arch == Arch::AArch64 ? Arm : Intel;
  const auto Found = Index.find(Import->str());
  if (Found == Index.end() || !Found->second)
    return std::nullopt;
  SourceCallTypeHint Result;
  Result.CallKind = SourceCallTypeHint::Kind::DarwinRuntimeCall;
  Result.TargetAddress = ImportSlot;
  Result.TargetName = Import->str();
  Result.Signature = *Found->second;
  return Result;
}
} // namespace neverd
