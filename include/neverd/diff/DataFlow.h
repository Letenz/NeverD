//===- DataFlow.h - Revision-bound SSA dependency evidence ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable SSA/CFG observations for region candidates and impact slices.
/// Physical register names and SSA numbering are not correspondence evidence.
/// Memory aliases, calls, unknown entries and incomplete control flow remain
/// explicit boundaries; these graphs never establish semantic equivalence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_DATAFLOW_H
#define NEVERD_DIFF_DATAFLOW_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd::diff {

enum FlowGap : uint32_t {
  FlowUnknownInput = 1u << 0,
  FlowMemoryAlias = 1u << 1,
  FlowCallEffects = 1u << 2,
  FlowIndirectControl = 1u << 3,
  FlowExceptionalControl = 1u << 4,
  FlowNativeCoverage = 1u << 5,
  FlowNoExit = 1u << 6,
};
inline constexpr uint32_t AllFlowGaps = (FlowNoExit << 1) - 1;
inline constexpr llvm::StringLiteral DataFlowProfile = "med-ssa-v2";
std::vector<llvm::StringRef> flowGapNames(uint32_t Mask);

/// An interrupted projection publishes no graph with missing definitions.
class DataFlowInterrupted : public std::runtime_error {
public:
  ComparisonState State;
  explicit DataFlowInterrupted(ComparisonState State)
      : std::runtime_error("dataflow projection " + stateName(State).str()),
        State(State) {}
};

struct DataFlowOptions {
  uint32_t MaxNodes = limits::diff::kDefaultFlowMaxNodes;
  uint32_t MaxBlocks = limits::diff::kDefaultFlowMaxBlocks;
  uint64_t MaxWork = limits::diff::kDefaultFlowMaxWork;
  unsigned Rounds = limits::diff::kDefaultFlowRounds;
  bool operator==(const DataFlowOptions &) const = default;
  std::optional<std::string> validate() const;
};

struct FlowOperand {
  enum class Kind : uint8_t { Node, Constant, Parameter, Unknown };
  Kind Type = Kind::Unknown;
  uint32_t Node = NoIndex;
  uint16_t Bytes = 0;
  /// Exact scalar bits or parameter position. An unknown's identity is only
  /// meaningful inside this snapshot and is excluded from correspondence keys.
  uint64_t Value = 0;
  /// Address provenance remains distinct from an equal-valued scalar.
  uint8_t Provenance = 0;
  uint64_t AddressOwner = 0;
  /// Incoming predecessor for a PHI operand; otherwise NoIndex.
  uint32_t Predecessor = NoIndex;
  bool operator==(const FlowOperand &) const = default;
};

struct FlowNode {
  std::string Opcode;
  uint16_t ResultBytes = 0;
  uint8_t MemoryOrdering = 0;
  uint8_t MemoryAddressSpace = 0;
  uint32_t Block = NoIndex;
  uint64_t Address = 0;
  /// -1 is synthetic; a PHI has block provenance, not an invented instruction.
  int32_t OriginSequence = -1;
  bool Phi = false;
  bool Commutative = false;
  uint32_t Gaps = 0;
  /// Recovered call arguments occupy this contiguous part of Inputs, after
  /// the raw operation inputs and before any preserved call-clobber prefix.
  /// NoIndex means no call-argument record; a known zero-argument record has
  /// a real Begin and Count zero. This does not establish callee/formal
  /// binding.
  uint32_t CallArgumentBegin = NoIndex;
  uint32_t CallArgumentCount = 0;
  std::vector<FlowOperand> Inputs;
  bool operator==(const FlowNode &) const = default;
};

struct FlowBlock {
  uint64_t Begin = 0;
  uint64_t End = 0;
  uint32_t Terminator = NoIndex;
  std::vector<uint32_t> Successors;
  uint32_t Gaps = 0;
  bool operator==(const FlowBlock &) const = default;
};

struct DataFlowGraph {
  std::string Revision;
  std::string Profile = DataFlowProfile.str();
  std::string ABI;
  uint64_t Function = 0;
  uint32_t EntryBlock = NoIndex;
  uint32_t Gaps = 0;
  std::vector<FlowNode> Nodes;
  std::vector<FlowBlock> Blocks;
  bool operator==(const DataFlowGraph &) const = default;
  std::optional<std::string> validate(const DataFlowOptions &Options) const;
};

struct FlowFingerprints {
  ComparisonState State = ComparisonState::Complete;
  uint64_t Work = 0;
  unsigned CompletedRounds = 0;
  /// Heuristic neighborhoods only. Hash equality cannot mark code unchanged.
  std::vector<uint64_t> Keys;
  /// One flag per key: the finite data neighborhood still reaches unvisited
  /// definitions, a cycle or an unknown effect. A closed data neighborhood
  /// still does not establish control/memory/ABI equivalence.
  std::vector<uint8_t> OpenDataFrontiers;
};
FlowFingerprints fingerprintDataFlow(const DataFlowGraph &Graph,
                                     const DataFlowOptions &Options = {},
                                     const ComparisonControl &Control = {});

enum class SliceDirection : uint8_t { Backward, Forward };
struct FlowSlice {
  ComparisonState State = ComparisonState::Complete;
  uint64_t Work = 0;
  uint32_t Gaps = 0;
  bool ControlDependencies = false;
  std::vector<uint32_t> Nodes;
};
/// Traverse explicit SSA dependencies and, when requested, postdominator-based
/// control dependencies. Loops without a path to an exit are a visible gap.
/// Alias-dependent memory edges and interprocedural effects are never guessed.
FlowSlice sliceDataFlow(const DataFlowGraph &Graph,
                        llvm::ArrayRef<uint32_t> Seeds,
                        SliceDirection Direction, bool IncludeControl,
                        const DataFlowOptions &Options = {},
                        const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_DATAFLOW_H
