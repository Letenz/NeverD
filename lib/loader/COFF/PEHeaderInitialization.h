//===- PEHeaderInitialization.h - PE loader writes -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded loader initialization evidence for PE header storage.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_COFF_PEHEADERINITIALIZATION_H
#define NEVERD_LOADER_COFF_PEHEADERINITIALIZATION_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>

namespace neverd {
struct PEHeaderPtrs;
namespace coff_loader {
struct PEHeaderInitializationWork {
  uint64_t Bytes = 0;
  uint64_t Records = 0;
};
/// Prove that supported initialization cannot replace header bytes other
/// than the separately retained optional-header ImageBase address.
/// This supplies no import identity, executable entry or ordinary-call effect.
bool hasBoundedPEHeaderInitialization(llvm::ArrayRef<uint8_t> Bytes,
                                      const PEHeaderPtrs &PE,
                                      PEHeaderInitializationWork &Work);
} // namespace coff_loader
} // namespace neverd
#endif
