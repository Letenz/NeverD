//===- UnicornExclusive.h - Shared ARM64 exclusive instruction bridge ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_UNICORN_EXCLUSIVE_H
#define NEVERD_EMULATION_UNICORN_EXCLUSIVE_H
#include "../../arch/aarch64/AArch64Exclusive.h"

#include <unicorn/unicorn.h>

namespace neverd::emulation {
/// Complete recognized exclusives before Unicorn enters its value-based
/// private monitor. Ordinary instructions continue through the engine.
llvm::Error
executeUnicornExclusive(uc_engine *, uint64_t PC, MemoryProjection &,
                        std::shared_ptr<RAMReservation> &,
                        const AArch64ExclusiveAccess &,
                        llvm::function_ref<int(CPURegister)> RegisterID);
/// A raw engine write hook precedes the write. Conservatively invalidate its
/// physical footprint after observers accept it, including unchanged bytes.
void observeUnicornRAMWrite(MemoryProjection &, uint64_t Address,
                            uint64_t Size);
} // namespace neverd::emulation
#endif
