//===- KernelWaits.h - Windows dispatcher wait policy --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELWAITS_H
#define NEVERD_EMULATION_WINDOWS_KERNELWAITS_H

#include <cstdint>

namespace neverd::emulation::kernel_wait {
#define NEVERD_KERNEL_WAIT_VALUE(Name, Value) constexpr uint32_t Name = Value;
#define NEVERD_KERNEL_WAIT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "KernelWaits.def"
#undef NEVERD_KERNEL_WAIT_TEXT
#undef NEVERD_KERNEL_WAIT_VALUE
} // namespace neverd::emulation::kernel_wait

#endif
