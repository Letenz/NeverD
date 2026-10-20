#include "gtest/gtest.h"

#include "neverd/ir/SourceVariableFlow.h"

using namespace neverd;
namespace {
SourceVariableFlow linear(unsigned Count = 4) {
  SourceVariableFlow Flow;
  Flow.Variables = {{"a", "u64", 8}, {"b", "u64", 8}};
  Flow.Nodes.resize(Count);
  for (unsigned I = 0; I + 1 < Count; ++I)
    Flow.Nodes[I].Successors = {I + 1};
  return Flow;
}
TEST(SourceVariableFlow, MapsWholeCopiesWithoutDestroyingLiveValues) {
  auto Flow = linear(5);
  Flow.Nodes[0].Writes = {0};
  Flow.Nodes[1].Reads = {0};
  Flow.Nodes[1].Writes = {1};
  Flow.Nodes[1].Copy = {{1, 0}};
  Flow.Nodes[2].Reads = {1};
  Flow.Nodes[3].Writes = {0};
  Flow.Nodes[4].Reads = {0};
  EXPECT_TRUE(canMapSourceVariables(Flow, 0, 1).empty());
  // A's new value is written while B's old copied value remains live.
  Flow.Nodes[2].Writes = {0};
  Flow.Nodes[2].Reads.clear();
  Flow.Nodes[3].Writes.clear();
  Flow.Nodes[3].Reads = {1};
  EXPECT_NE(canMapSourceVariables(Flow, 0, 1).find("overlapping"),
            std::string::npos);
}
TEST(SourceVariableFlow, RefusesUnrelatedEscapingAndIncompatibleValues) {
  auto Flow = linear();
  Flow.Nodes[0].Writes = {0};
  Flow.Nodes[1].Reads = {0};
  Flow.Nodes[2].Writes = {1};
  Flow.Nodes[3].Reads = {1};
  EXPECT_NE(canMapSourceVariables(Flow, 0, 1).find("copy"), std::string::npos);
  Flow.Variables[0].Eligible = false;
  Flow.Variables[0].Refusal = "The address escapes";
  EXPECT_EQ(canMapSourceVariables(Flow, 0, 1), "The address escapes");
  Flow.Variables[0].Eligible = true;
  Flow.Variables[1].Bytes = 4;
  EXPECT_NE(canMapSourceVariables(Flow, 0, 1).find("width"), std::string::npos);
}
TEST(SourceVariableFlow, SplitsIndependentReachingDefinitionLifetimes) {
  auto Flow = linear();
  Flow.Nodes[0].Writes = {0};
  Flow.Nodes[1].Reads = {0};
  Flow.Nodes[2].Writes = {0};
  Flow.Nodes[3].Reads = {0};
  const auto Split = splitSourceVariable(Flow, 0);
  ASSERT_TRUE(Split) << Split.Refusal;
  EXPECT_EQ(Split.Partitions, 2u);
  EXPECT_EQ(Split.Definitions[0], Split.Uses[1]);
  EXPECT_EQ(Split.Definitions[2], Split.Uses[3]);
  EXPECT_NE(Split.Uses[1], Split.Uses[3]);
}
TEST(SourceVariableFlow, KeepsLoopCarriedDefinitionsTogether) {
  auto Flow = linear(6);
  Flow.Nodes[0].Writes = {0};
  Flow.Nodes[1].Reads = {0};
  Flow.Nodes[1].Successors = {2, 4};
  Flow.Nodes[2].Writes = {0};
  Flow.Nodes[2].Successors = {1};
  Flow.Nodes[4].Writes = {0};
  Flow.Nodes[5].Reads = {0};
  const auto Split = splitSourceVariable(Flow, 0);
  ASSERT_TRUE(Split) << Split.Refusal;
  EXPECT_EQ(Split.Definitions[0], Split.Definitions[2]);
  EXPECT_NE(Split.Definitions[0], Split.Definitions[4]);
}
TEST(SourceVariableFlow, RefusesUndefinedPathsAndMergedOnlyDefinitions) {
  auto Flow = linear(6);
  Flow.Nodes[0].Successors = {1, 2};
  Flow.Nodes[1].Writes = {0};
  Flow.Nodes[1].Successors = {3};
  Flow.Nodes[2].Successors = {3};
  Flow.Nodes[3].Reads = {0};
  Flow.Nodes[4].Writes = {0};
  Flow.Nodes[5].Reads = {0};
  EXPECT_NE(splitSourceVariable(Flow, 0).Refusal.find("complete definition"),
            std::string::npos);
  Flow.Nodes[2].Writes = {0};
  Flow.Nodes[4].Writes.clear();
  EXPECT_NE(splitSourceVariable(Flow, 0).Refusal.find("one used lifetime"),
            std::string::npos);
}
TEST(SourceVariableFlow, RefusesIncompleteGraphsAndEntryParameterSplits) {
  auto Flow = linear();
  Flow.Nodes[0].Successors = {999};
  EXPECT_FALSE(splitSourceVariable(Flow, 0));
  Flow.Nodes[0].Successors = {1};
  Flow.Variables[0].EntryValue = true;
  EXPECT_NE(splitSourceVariable(Flow, 0).Refusal.find("parameters"),
            std::string::npos);
}
} // namespace
