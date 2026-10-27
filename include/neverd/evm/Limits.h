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

#ifndef NEVERD_EVM_LIMITS_H
#define NEVERD_EVM_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::evm {

// lib/evm/bytecode/EVMMetadata.cpp
/// The most items one container may declare, and the most containers one value
/// may nest. A footer is a handful of small items inside at most a sequence, a
/// map, and a version array; these bounds only keep a corrupt count from naming
/// a huge container and a hostile one from recursing.
inline constexpr uint64_t kEVMMetadataMaxItems = 32;
inline constexpr unsigned kEVMMetadataMaxDepth = 4;

} // namespace neverd::limits::evm

#endif // NEVERD_EVM_LIMITS_H
