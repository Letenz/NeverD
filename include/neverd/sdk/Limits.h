//===- Limits.h - SDK resource limits and defaults ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SDK request bounds, retained-state limits and default work budgets.
/// Keep SDK policy values here; binary layouts and protocol constants belong
/// to their owning models. Shared pipeline limits live in neverd/Limits.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_LIMITS_H
#define NEVERD_SDK_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::sdk {

// Query paging and decoding.

/// Instructions read from a function without a recorded size. Bounds a
/// corrupt or missing extent, including the function's reachable jump paths.
inline constexpr uint64_t kMaxUnsizedInstructions = 65536;
inline constexpr int kMaxFunctionsPerPage = 4096;
inline constexpr int kMaxSwitchFunctionsPerPage = 4096;
inline constexpr int kMaxDataSlotsPerQuery = 65536;
inline constexpr int kMaxSourceEntriesPerQuery = 65536;
inline constexpr int kMaxStringRowsPerPage = 65536;
inline constexpr int kMaxDecodedTextBytes = 65536;
inline constexpr size_t kStringSearchWindowBytes = 65536;
inline constexpr unsigned kDefaultStringMinLength = 4;
inline constexpr size_t kShortInstructionByteColumnWidth = 18;
inline constexpr int kHexDumpBytesPerLine = 16;
inline constexpr size_t kMaxSearchOptionsBytes = 32768;
inline constexpr size_t kMaxSearchPatternBytes = 4096;
inline constexpr int64_t kDefaultSearchResults = 256;
inline constexpr int64_t kMaxSearchResults = 4096;
inline constexpr uint64_t kMaxOwnedResourceStringBytes = 64 * 1024 * 1024;

// Call paths and trees.

inline constexpr unsigned kMaxCallPathDepth = 64;
inline constexpr unsigned kDefaultCallPathDepth = 16;
inline constexpr unsigned kDefaultCallTreeDepth = 1;
inline constexpr unsigned kMaxCallPaths = 1000;
inline constexpr unsigned kDefaultCallPaths = 32;
inline constexpr unsigned kMaxCallGraphListed = 100000;
inline constexpr unsigned kDefaultCallGraphListed = 4096;
inline constexpr unsigned kMaxCallPathWaypoints = 64;
/// Steps before one segment's simple-path enumeration reports incomplete.
inline constexpr uint64_t kCallPathStepBudget = 4'000'000;

// Local types and persisted user edits.

inline constexpr size_t kMaxTypeFitAccesses = 4096;
inline constexpr size_t kMaxTypeFitTextBytes = 256;
inline constexpr int64_t kMaxTypeFitCandidates = 500;
inline constexpr int64_t kMaxTypeFitAccessBytes = 1 << 16;
inline constexpr unsigned kMaxLocalTypeConversionDepth = 16;
/// Admission and persistence use the same bound for function declarations.
inline constexpr size_t kMaxFunctionDeclarationBytes = 4096;
inline constexpr size_t kMaxUserNameBytes = 4096;
inline constexpr int kMaxFormattedOperands = 8;
inline constexpr size_t kMaxOperandTypeNameBytes = 512;
inline constexpr size_t kMaxCallSnapshotOperations = 200000;
inline constexpr uint64_t kMaxCallSnapshotBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxCallVariadicTypes = 32;
inline constexpr size_t kMaxSourceOptionsBytes = 256 * 1024;
inline constexpr size_t kMaxSourceEditIdBytes = 256;
inline constexpr size_t kMaxSourceSnapshotBytes = 128;
inline constexpr size_t kMaxSourceVariableTypes = 256;
inline constexpr size_t kMaxSourceVariableAssociations = 64;
inline constexpr size_t kMaxSourceCallEdits = 32;
inline constexpr size_t kMaxSourceExpressionSnapshotNodes = 100000;
inline constexpr uint64_t kMaxSourceExpressionSnapshotBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxStackFramePoints = 256;
inline constexpr size_t kMaxStackFrameVariables = 256;
inline constexpr size_t kMaxStackVariableNameBytes = 128;
inline constexpr size_t kMaxStackFrameOptionsBytes = 128 * 1024;
inline constexpr size_t kMaxRegisterFlowOptionsBytes = 4096;
inline constexpr size_t kMaxRegisterFlowBlocks = 4096;
inline constexpr size_t kMaxRegisterFlowInstructions = 20000;
inline constexpr size_t kMaxEnumTypeNameBytes = 512;
inline constexpr size_t kMaxEnumValues = 4096;
inline constexpr size_t kMaxSwiftMethodSelections = 65536;
inline constexpr unsigned kMaxSourcePatchEntryPadding = 1024;

// Retained views and diagnostic output.

inline constexpr size_t kMaxFunctionLlvmModules = 8;
inline constexpr size_t kMaxFunctionSources = 8;
inline constexpr size_t kMaxSourceProjectionDiagnostics = 4096;
inline constexpr size_t kMaxNativeSourceDependencyCalls = 1000000;
inline constexpr size_t kMaxLLVMCommentSections = 6;
inline constexpr size_t kMaxLLVMCommentReaches = 16;
inline constexpr size_t kLLVMCommentWidth = 78;
inline constexpr size_t kDebugFileSampleNames = 8;

// Symbolic C API defaults.

inline constexpr unsigned kDefaultSymbolicWidth = 32;
inline constexpr unsigned kDefaultOptimizationRounds = 8;
/// The C surface is bounded by default even when the C++ solver is not.
/// Callers may explicitly select exhaustive work through their options.
inline constexpr uint64_t kDefaultSolverMaxConflicts = uint64_t{1} << 18;
inline constexpr uint64_t kDefaultSolverMaxPropagations = uint64_t{1} << 24;
inline constexpr uint64_t kDefaultSolverMaxWatchVisits = uint64_t{1} << 26;
inline constexpr uint32_t kDefaultSynthesisTimeoutMs = 1000;

// Incremental immediate search.

inline constexpr size_t kImmediateSearchStepInstructions = 4096;
inline constexpr size_t kImmediateSearchStepBytes = 65536;
inline constexpr size_t kMaxImmediateSearchGaps = 64;
inline constexpr size_t kMaxImmediateSearchOperands = 2000000;
inline constexpr uint64_t kMaxImmediateSearchCodeBytes = 512ULL * 1024 * 1024;

// Decode benchmark sampling.

/// Cap timed input so large images still give a quick representative probe.
inline constexpr uint64_t kDecodeBenchmarkBytes = 16ull << 20;
inline constexpr size_t kDecodeBenchmarkChunkBytes = 64ull << 10;

// Native source projection.

/// HighIR expression visits in ordinary, evaluated and bounded value walks.
inline constexpr size_t kSourceExpressionWork = 100000;
inline constexpr size_t kSourceEvaluationWork = 1000000;
inline constexpr size_t kSourceValueWork = 4096;
inline constexpr size_t kSourceSliceWork = 128;
inline constexpr size_t kSourceFrameWork = 10000;
inline constexpr size_t kSourceConstructorWork = 262144;
inline constexpr size_t kSourceSetterWork = 1U << 18;
inline constexpr size_t kSwiftOnceSliceWork = 1024;
inline constexpr size_t kSwiftOnceValueWork = 256;
inline constexpr size_t kSourceBindingSmallWork = 256;
inline constexpr size_t kSourceBindingCallWork = 1000;
inline constexpr size_t kSourceBindingOccurrenceWork = 16384;
inline constexpr size_t kSourceBindingFieldWork = 65536;
inline constexpr size_t kSourceMetadataScanWork = 1000000;
inline constexpr size_t kMaxBlockSourceWork = 4000000;
inline constexpr size_t kMaxBlockSourceFacts = 262144;
inline constexpr size_t kMaxSourceBindingExports = 262144;
inline constexpr size_t kMaxSourceBindingDescriptors = 8;

/// Swift mangling reader profiles for a short identity and a full binding.
inline constexpr size_t kSwiftIdentityInputBytes = 1024;
inline constexpr size_t kSwiftIdentityNodes = 128;
inline constexpr unsigned kSwiftIdentityDepth = 24;
inline constexpr size_t kSwiftIdentityMemoryBytes = 65536;
inline constexpr size_t kSwiftIdentityOperations = 10000;
inline constexpr size_t kSwiftBindingInputBytes = 8000;
inline constexpr size_t kSwiftBindingNodes = 1024;
inline constexpr unsigned kSwiftBindingDepth = 64;
inline constexpr size_t kSwiftBindingMemoryBytes = 1024 * 1024;
inline constexpr size_t kSwiftBindingOperations = 100000;

} // namespace neverd::sdk

#endif // NEVERD_SDK_LIMITS_H
