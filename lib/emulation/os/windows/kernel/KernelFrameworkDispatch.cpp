//===- KernelFrameworkDispatch.cpp - KMDF call registration and admission ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelFramework.h"
#include "KernelScheduler.h"

#include "llvm/ADT/StringMap.h"

namespace neverd::emulation {
namespace {
#define NEVERD_KERNEL_NAME(Name, Value)                                        \
  constexpr llvm::StringLiteral Name = Value;
#include "KernelNames.def"
#undef NEVERD_KERNEL_NAME

enum class CallScope { Loader, Bound, Unload };
enum class IRQLPolicy { Passive, Dispatch, Any, Custom };

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}

} // namespace

struct KernelFramework::Routine {
  using Handler = llvm::Expected<uint64_t> (*)(KernelFramework &,
                                               llvm::StringRef, Binding *,
                                               llvm::ArrayRef<uint64_t>,
                                               uint8_t);
  unsigned ArgumentCount;
  CallScope Scope;
  IRQLPolicy Policy;
  Handler Invoke;
};

const KernelFramework::Routine *
KernelFramework::lookupRoutine(const KernelExportRegistry::Export &Export) {
  static const llvm::StringMap<Routine> Functions{
#define NEVERD_FRAMEWORK_API(Symbol, Count, Method, Policy)                    \
  {#Symbol,                                                                    \
   {Count, CallScope::Bound, IRQLPolicy::Policy,                               \
    [](KernelFramework &Model, llvm::StringRef Name, Binding *B,               \
       llvm::ArrayRef<uint64_t> A, uint8_t IRQL) -> llvm::Expected<uint64_t> { \
      return Model.Method(Name, *B, A, IRQL);                                  \
    }}},
#include "KernelFrameworkAPIs.def"
#undef NEVERD_FRAMEWORK_API
      {FrameworkUnloadRoutine,
       {1, CallScope::Unload, IRQLPolicy::Custom,
        [](KernelFramework &Model, llvm::StringRef Name, Binding *B,
           llvm::ArrayRef<uint64_t> A,
           uint8_t IRQL) { return Model.callUnload(Name, *B, A, IRQL); }}},
  };
  static const llvm::StringMap<Routine> Loaders{
#define NEVERD_FRAMEWORK_LOADER_API(Name, Count, Method)                       \
  {#Name,                                                                      \
   {Count, CallScope::Loader, IRQLPolicy::Passive,                             \
    [](KernelFramework &Model, llvm::StringRef, Binding *,                     \
       llvm::ArrayRef<uint64_t> A, uint8_t) { return Model.Method(A); }}},
#include "KernelFrameworkLoaderAPIs.def"
#undef NEVERD_FRAMEWORK_LOADER_API
  };
  const bool Bound =
      Export.Kind == KernelExportRegistry::ExportKind::FrameworkFunction;
  if (!Bound && Export.Module != FrameworkLoaderProvider)
    return nullptr;
  const auto &Table = Bound ? Functions : Loaders;
  auto I = Table.find(Export.Name);
  return I == Table.end() ? nullptr : &I->second;
}

std::optional<unsigned>
KernelFramework::argumentCount(const KernelExportRegistry::Export &Export) {
  if (const auto *Routine = lookupRoutine(Export))
    return Routine->ArgumentCount;
  return std::nullopt;
}

llvm::Expected<uint64_t>
KernelFramework::callImpl(const KernelExportRegistry::Export &Export,
                          llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  const auto *Routine = lookupRoutine(Export);
  if (!Routine || Routine->ArgumentCount != A.size())
    return invalid("unsupported framework routine or argument count");
  if (Routine->Scope == CallScope::Loader) {
    if (IRQL)
      return invalid("framework binding requires PASSIVE_LEVEL");
    return Routine->Invoke(*this, Export.Name, nullptr, A, IRQL);
  }
  auto BI = Bindings.find(Export.Binding);
  if (BI == Bindings.end() || BI->second.Unbound)
    return invalid("table entry belongs to an unbound framework instance");
  auto &B = BI->second;
  if (Routine->Scope == CallScope::Bound && A[0] != B.Globals)
    return invalid("framework function received another binding's globals");
  switch (Routine->Policy) {
  case IRQLPolicy::Passive:
  case IRQLPolicy::Dispatch:
    if (IRQL > (Routine->Policy == IRQLPolicy::Dispatch
                    ? scheduler::DispatchLevel
                    : scheduler::PassiveLevel))
      return invalid("framework routine requires PASSIVE_LEVEL or a supported "
                     "DISPATCH_LEVEL operation");
    break;
  case IRQLPolicy::Any:
  case IRQLPolicy::Custom:
    break;
  }
  return Routine->Invoke(*this, Export.Name, &B, A, IRQL);
}
} // namespace neverd::emulation
