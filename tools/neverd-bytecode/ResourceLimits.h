//===- ResourceLimits.h - Resource limits and defaults -----*- C++ -*-===//
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

#ifndef NEVERD_TOOLS_NEVERD_BYTECODE_RESOURCELIMITS_H
#define NEVERD_TOOLS_NEVERD_BYTECODE_RESOURCELIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::bytecode_tool {

// tools/neverd-bytecode/BytecodeObserve.cpp
inline constexpr uint64_t kBytecodeObserveMaxSteps = 50000;
inline constexpr uint64_t kBytecodeObserveMaxMilliseconds = 10000;
inline constexpr int kBytecodeObserveFeasibleMaxConflicts = 500;
inline constexpr int kBytecodeObserveFeasibleMaxPropagations = 50000;
inline constexpr int kBytecodeObserveFeasibleMaxWatchVisits = 500000;
inline constexpr int kBytecodeObserveFeasibleMaxGates = 65536;

// tools/neverd-bytecode/BytecodeObserve.cpp
inline constexpr int kBytecodeObserveReadFileBGetBufferSizeLimit =
    64 * 1024 * 1024;
inline constexpr int kBytecodeObserveDescribeCReadabilityCostRLimit = 80;
inline constexpr int kBytecodeObserveDescribeSSizeLimit = 300;
inline constexpr int kBytecodeObserveInputStringTextSizeLimit = 512;
inline constexpr int kBytecodeObserveApplyMemoryEffectCountLimit = 4096;
inline constexpr int kBytecodeObserverBytesSizeLimit = 1024 * 1024;
inline constexpr int kBytecodeObserverMaxStepsLimit = 1000000;
inline constexpr int kBytecodeObserverMaxPathsLimit = 256;
inline constexpr int kBytecodeObserverMaxVisitsLimit = 100000;
inline constexpr int kBytecodeObserverMaxMillisecondsLimit = 600000;
inline constexpr int kBytecodeObserveRunCNumNodesLimit = 1000000;
inline constexpr int kBytecodeObserveRunPFramesSizeLimit = 32;
inline constexpr int kBytecodeObserveRunSymbolFlowsSizeLimit = 4096;
inline constexpr int kMaxAnnotatedCallComments = 96;
inline constexpr int kMaxAnnotatedEvidenceComments = 128;

// tools/neverd-bytecode/neverd-bytecode.cpp
inline constexpr int kNeverdBytecodeReadBufferGetBufferSizeLimit =
    64 * 1024 * 1024;

// tools/neverd-bytecode/BytecodeObserve.cpp
inline constexpr int kBytecodeObserveStringILimit = 512;
inline constexpr int kBytecodeObserveCommentTextILimit = 1000;

} // namespace neverd::limits::bytecode_tool

#endif // NEVERD_TOOLS_NEVERD_BYTECODE_RESOURCELIMITS_H
