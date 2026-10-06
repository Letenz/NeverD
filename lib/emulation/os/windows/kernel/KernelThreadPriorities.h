//===- KernelThreadPriorities.h - Thread priority policy ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNEL_THREAD_PRIORITIES_H
#define NEVERD_EMULATION_KERNEL_THREAD_PRIORITIES_H

#include <cstdint>

namespace neverd::emulation::thread_priority {
#define NEVERD_THREAD_PRIORITY_VALUE(Name, Value)                              \
  constexpr int32_t Name = Value;
#define NEVERD_THREAD_PRIORITY_TEXT(Name, Value) constexpr char Name[] = Value;
#include "KernelThreadPriorities.def"
#undef NEVERD_THREAD_PRIORITY_TEXT
#undef NEVERD_THREAD_PRIORITY_VALUE
} // namespace neverd::emulation::thread_priority

#endif
