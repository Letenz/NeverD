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
class PhysicalMemory;
class X64Machine;
class AArch64Machine;

llvm::Expected<std::unique_ptr<X64Machine>> createKvmMachine(uint8_t *Backing,
                                                             uint64_t Size);
llvm::Expected<std::unique_ptr<X64Machine>> createWhpMachine(uint8_t *Backing,
                                                             uint64_t Size);
llvm::Expected<std::unique_ptr<X64Machine>>
createUnicornX64Machine(PhysicalMemory &Memory);
llvm::Expected<std::unique_ptr<AArch64Machine>>
createKvmAArch64Machine(PhysicalMemory &Memory);
llvm::Expected<std::unique_ptr<AArch64Machine>>
createWhpAArch64Machine(PhysicalMemory &Memory);
llvm::Expected<std::unique_ptr<AArch64Machine>>
createUnicornAArch64Machine(PhysicalMemory &Memory);
} // namespace neverd::emulation
#endif
