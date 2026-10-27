//===- ModuleResolver.cpp - Shared automatic/manual supply path
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared automatic/manual supply path.
///

#include "neverd/emulation/ModuleResolver.h"

namespace neverd::emulation {
class ModuleResolverGuard {
public:
  explicit ModuleResolverGuard(ModuleBindings &Bindings) : Bindings(Bindings) {}
  bool acquire() {
    if (Bindings.Busy)
      return false;
    Bindings.Busy = true;
    Held = true;
    return true;
  }
  ~ModuleResolverGuard() {
    if (Held)
      Bindings.Busy = false;
  }

private:
  ModuleBindings &Bindings;
  bool Held = false;
};
llvm::Expected<ModuleResolutionKind>
resolveModuleRequest(ModuleBindings &Bindings, const ModuleResolver &Resolver,
                     const ModuleMappingBuilder &Builder) {
  const auto Request = Bindings.pendingRequest();
  if (!Request || !Resolver)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "module resolver requires a pending request and callback");
  if (auto E = Request->Space->validateBacking(0, 0))
    return std::move(E);
  auto Result = [&]() -> llvm::Expected<ModuleResolution> {
    ModuleResolverGuard Guard(Bindings);
    if (!Guard.acquire())
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "module resolver is already active");
    return Resolver(*Request);
  }();
  if (!Result)
    return Result.takeError();
  switch (Result->Kind) {
  case ModuleResolutionKind::Found:
    if (auto E = Bindings.supply(Request->Token, Result->Bindings, Builder))
      return std::move(E);
    return Result->Kind;
  case ModuleResolutionKind::NeedInput:
  case ModuleResolutionKind::NotFound:
    if (!Result->Bindings.empty())
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "unresolved module result contains bindings");
    return Result->Kind;
  }
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "invalid module resolution kind");
}
} // namespace neverd::emulation
