//===- MachineFactories.h - Concrete CPU transport construction ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_BACKENDS_MACHINEFACTORIES_H
#define NEVERD_EMULATION_BACKENDS_MACHINEFACTORIES_H
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>

namespace neverd::emulation {
class MemoryProjection;
class X64Machine;
class AArch64Machine;

llvm::Expected<std::unique_ptr<X64Machine>>
createKvmMachine(MemoryProjection &Memory);
llvm::Expected<std::unique_ptr<X64Machine>>
createWhpMachine(MemoryProjection &Memory);
llvm::Expected<std::unique_ptr<X64Machine>>
createUnicornX64Machine(MemoryProjection &Memory, bool UserMode = false);
llvm::Expected<std::unique_ptr<AArch64Machine>>
createKvmAArch64Machine(MemoryProjection &Memory);
llvm::Expected<std::unique_ptr<AArch64Machine>>
createWhpAArch64Machine(MemoryProjection &Memory);
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(MemoryProjection &Memory, bool UserMode = false);
} // namespace neverd::emulation
#endif
