//===- X64MSRRead.h - Bounded MSR read encoding and selectors -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MSR reads whose declared value does not replace other owned CPU state.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_X64MSRREAD_H
#define NEVERD_EMULATION_X64MSRREAD_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace neverd::emulation::x64::msr {
// Intel SDM Vol. 4: the IA-32e system-call target is an opaque address here.
// SYSCALL/SYSRET and WRMSR remain outside the checked driver environment.
// In particular, declarations cannot replace FS/GS, EFER, CET or TSC owners.
inline constexpr uint32_t LSTAR = 0xc0000082;
inline bool supportsDeclaredRead(uint32_t Index) { return Index == LSTAR; }

inline bool canonicalRead(llvm::ArrayRef<uint8_t> Bytes) {
  return Bytes.size() == 2 && Bytes[0] == 0x0f && Bytes[1] == 0x32;
}
} // namespace neverd::emulation::x64::msr

#endif
