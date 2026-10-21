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

#ifndef NEVERD_TRANSLATE_LIMITS_H
#define NEVERD_TRANSLATE_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::translate {

// lib/translate/TranslationObjectCompiler.cpp
inline constexpr int
    kTranslationObjectCompilerTranslationSemanticPolicyV1BoundedMaxRounds = 8;

} // namespace neverd::limits::translate

#endif // NEVERD_TRANSLATE_LIMITS_H
