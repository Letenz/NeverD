//===- UnicornUnavailable.cpp - Native-only build boundary ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/aarch64/AArch64Machine.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../MachineFactories.h"
namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>>
createUnicornX64Machine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::UnicornDisabled);
}
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::UnicornDisabled);
}
} // namespace neverd::emulation
