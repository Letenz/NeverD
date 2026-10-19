//===- ModuleResolver.h - Stopped resource resolution ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stopped resource resolution.
///

#ifndef NEVERD_EMULATION_MODULERESOLVER_H
#define NEVERD_EMULATION_MODULERESOLVER_H
#include "neverd/emulation/ModuleBinding.h"

namespace neverd::emulation {
enum class ModuleResolutionKind { Found, NeedInput, NotFound };
struct ModuleResolution {
  ModuleResolutionKind Kind = ModuleResolutionKind::NeedInput;
  std::vector<ModuleBinding> Bindings;
};
using ModuleResolver =
    std::function<llvm::Expected<ModuleResolution>(const ModuleRequest &)>;
/// Invoke once, outside CPU callbacks, then submit through the manual supply
/// path. NeedInput/NotFound retain the request. There is no implicit file
/// search, download, retry loop or CPU resume. Resolver may register sources;
/// it must not mutate the execution's bindings or execute any guest CPU.
llvm::Expected<ModuleResolutionKind>
resolveModuleRequest(ModuleBindings &Bindings, const ModuleResolver &Resolver,
                     const ModuleMappingBuilder &Builder);
} // namespace neverd::emulation
#endif
