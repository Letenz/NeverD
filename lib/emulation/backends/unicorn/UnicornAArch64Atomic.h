//===- UnicornAArch64Atomic.h - Shared ARM64 atomic instruction bridge
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_UNICORN_AARCH64ATOMIC_H
#define NEVERD_EMULATION_UNICORN_AARCH64ATOMIC_H
#include "../../arch/aarch64/AArch64Exclusive.h"

#include <unicorn/unicorn.h>

namespace neverd::emulation {
/// Complete atomic and exclusive instructions under the shared physical lease
/// and monitor authority. Other instructions continue through the engine.
llvm::Error
executeUnicornAArch64Atomic(uc_engine *, uint64_t PC, MemoryProjection &,
                            std::shared_ptr<RAMReservation> &,
                            const AArch64AtomicAccess &,
                            llvm::function_ref<int(CPURegister)> RegisterID);
/// A raw engine write hook precedes the write. Conservatively invalidate its
/// physical footprint after observers accept it, including unchanged bytes.
void observeUnicornRAMWrite(MemoryProjection &, uint64_t Address,
                            uint64_t Size);
} // namespace neverd::emulation
#endif
