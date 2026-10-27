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

#ifndef NEVERD_MODULES_LIMITS_H
#define NEVERD_MODULES_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::modules {
namespace input_limit {
#define NEVERD_MODULE_INPUT_LIMIT(Name, CName, Value)                          \
  inline constexpr uint64_t Name = Value;
#include "neverd/modules/ModuleInputLimits.def"
#undef NEVERD_MODULE_INPUT_LIMIT
} // namespace input_limit
} // namespace neverd::modules

namespace neverd::limits::modules {

inline constexpr int kMaxModuleInstances = 4096;
inline constexpr unsigned long long kMaxModuleImageBytes = 8ULL << 30;
inline constexpr unsigned long long kMaxModuleDebugBytes = 8ULL << 30;
inline constexpr int kMaxModuleSymbols = 10000000;

// lib/modules/ModuleImage.cpp
inline constexpr int kModuleImageLookupSymbolNameSizeLimit =
    static_cast<int>(::neverd::modules::input_limit::SymbolBytes);

// lib/modules/ModuleOptionsJSON.cpp
inline constexpr unsigned long long
    kModuleOptionsJSONNormalizeModuleOptionsLTotalSourceBytesLimit =
        (64ULL << 30);
inline constexpr int kModuleOptionsJSONModuleOptionsFromJSONArraySizeLimit =
    kMaxModuleInstances;

} // namespace neverd::limits::modules

#endif // NEVERD_MODULES_LIMITS_H
