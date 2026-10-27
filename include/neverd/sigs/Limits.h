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

#ifndef NEVERD_SIGS_LIMITS_H
#define NEVERD_SIGS_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::sigs {
#define NEVERD_SIGS_LIMIT(Name, Value) inline constexpr auto k##Name = Value;
#include "neverd/sigs/SignatureLimits.def"
} // namespace neverd::limits::sigs

#endif // NEVERD_SIGS_LIMITS_H
