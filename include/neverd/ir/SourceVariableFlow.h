//===- SourceVariableFlow.h - Source-local lifetime proofs ----*- C++ -*-===//
#ifndef NEVERD_IR_SOURCEVARIABLEFLOW_H
#define NEVERD_IR_SOURCEVARIABLEFLOW_H

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
/// A source projection's scalar identity, not a machine register or an ABI
/// assertion. Adapters reject escaping/partial/unknown storage before analysis.
struct SourceFlowVariable {
  std::string Id;
  std::string TypeKey;
  unsigned Bytes = 0;
  bool EntryValue = false;
  bool Eligible = true;
  std::string Refusal;
};
struct SourceVariableFlowNode {
  std::vector<size_t> Successors;
  std::vector<unsigned> Reads;
  std::vector<unsigned> Writes;
  /// A whole-value copy, destination then source, evaluated read-before-write.
  std::optional<std::pair<unsigned, unsigned>> Copy;
};
struct SourceVariableFlow {
  std::vector<SourceFlowVariable> Variables;
  std::vector<SourceVariableFlowNode> Nodes;
  size_t Entry = 0;
  std::string Refusal;
};
struct SourceVariableSplit {
  static constexpr unsigned None = std::numeric_limits<unsigned>::max();
  unsigned Partitions = 0;
  std::vector<unsigned> Definitions;
  std::vector<unsigned> Uses;
  std::string Refusal;
  explicit operator bool() const { return Partitions > 1 && Refusal.empty(); }
};
/// Empty on success. Requires a whole-value copy relationship, matching scalar
/// types, complete definitions and no interfering live ranges on any CFG path.
std::string canMapSourceVariables(const SourceVariableFlow &Flow, unsigned From,
                                  unsigned To);
/// Partition complete definitions by their reaching uses. Definitions meeting
/// at a read remain in one partition; disjoint used lifetimes may separate.
/// No result is usable after a budget or graph/definition failure.
SourceVariableSplit splitSourceVariable(const SourceVariableFlow &Flow,
                                        unsigned Variable);
} // namespace neverd
#endif
