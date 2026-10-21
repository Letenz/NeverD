//===- Limits.h - Component limits and defaults -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNPACK_LIMITS_H
#define NEVERD_UNPACK_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::unpack {

// lib/unpack/os/windows/RuntimeState.cpp
inline constexpr uint64_t kRuntimeStateMaxBacking = 32 * 1024 * 1024;
inline constexpr uint64_t kRuntimeStateMaxBindings = 16384;

// lib/unpack/os/windows/RuntimeState.cpp
inline constexpr int kRuntimeStateValidNameSSizeLimit = 4096;
inline constexpr int kRuntimeStateRestoreRuntimeSAllocationsSizeLimit = 8192;
inline constexpr int kRuntimeStateRestoreRuntimeSPrivateHeapsSizeLimit = 4096;
inline constexpr int kRuntimeStateRestoreRuntimeSCriticalSectionsSizeLimit =
    4096;
inline constexpr int kRuntimeStateRestoreRuntimeSFiberSlotsSizeLimit = 64;
inline constexpr int kRuntimeStateRestoreRuntimeSVirtualAllocationsSizeLimit =
    8192;

// lib/unpack/os/windows/RuntimeState.cpp
inline constexpr int kMaxCriticalSectionRecursion = 1048576;

// lib/unpack/format/pe/PERuntime.cpp
inline constexpr int kMaxRuntimeTLSCallbacks = 4096;

} // namespace neverd::limits::unpack

#endif // NEVERD_UNPACK_LIMITS_H
