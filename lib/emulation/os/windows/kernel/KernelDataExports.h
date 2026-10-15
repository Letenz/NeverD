//===- KernelDataExports.h - Kernel data export layout --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe the read-only kernel data export layout.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_KERNELDATAEXPORTS_H
#define NEVERD_EMULATION_KERNELDATAEXPORTS_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd::emulation::kernel_data {
struct Export {
  llvm::StringLiteral Name;
  uint32_t Offset;
  uint32_t Size;
};
inline constexpr Export Exports[] = {
#define NEVERD_KERNEL_DATA_EXPORT(Name, Offset, Size, Value)                   \
  {#Name, Offset, Size},
#include "KernelDataExports.def"
#undef NEVERD_KERNEL_DATA_EXPORT
};
inline const Export *lookup(llvm::StringRef Name) {
  for (const auto &Entry : Exports)
    if (Entry.Name == Name)
      return &Entry;
  return nullptr;
}
} // namespace neverd::emulation::kernel_data
#endif
