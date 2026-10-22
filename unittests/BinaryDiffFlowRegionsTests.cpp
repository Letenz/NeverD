//===- BinaryDiffFlowRegionsTests.cpp - SSA region evidence invariants ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe split/merge/clone alternatives without double-counting overlapping
/// neighborhoods or silently admitting weak, stopped or incompatible evidence.
//===----------------------------------------------------------------------===//

#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/FlowRegionsJSON.h"

#include "llvm/ADT/STLExtras.h"

#include <atomic>
#include <gtest/gtest.h>
#include <set>

using namespace neverd::diff;
namespace {
DataFlowGraph chains(uint64_t Address, std::initializer_list<uint64_t> Salts) {
  DataFlowGraph G;
  G.Revision = "fixture-revision";
  G.ABI = "fixture-abi";
  G.Function = Address;
  G.EntryBlock = 0;
  G.Blocks.push_back({Address, Address + 0x100});
  for (auto Salt : Salts) {
    const auto Start = uint32_t(G.Nodes.size());
    for (uint32_t I = 0; I < 5; ++I) {
      FlowNode N;
      N.Opcode = I % 2 ? "INT_MULT" : "INT_ADD";
      N.ResultBytes = 8;
      N.Block = 0;
      N.Address = Address + G.Nodes.size();
      N.OriginSequence = G.Nodes.size();
      FlowOperand Input;
      Input.Type = I ? FlowOperand::Kind::Node : FlowOperand::Kind::Parameter;
      Input.Node = I ? Start + I - 1 : NoIndex;
      Input.Bytes = 8;
      N.Inputs.push_back(Input);
      Input.Type = FlowOperand::Kind::Constant;
      Input.Node = NoIndex;
      Input.Value = Salt + I;
      N.Inputs.push_back(Input);
      G.Nodes.push_back(std::move(N));
    }
  }
  return G;
}
Program program(std::initializer_list<DataFlowGraph> Graphs) {
  ProgramIdentity Id;
  Id.Architecture = "x86_64";
  Id.ABI = "fixture-abi";
  Id.Revision = "fixture-revision";
  Id.SHA256 = std::string(64, 'a');
  Id.Bits = 64;
  Id.Endian = "little";
  ProgramBuilder B(Id);
  auto Evidence = std::make_shared<DataFlowEvidence>();
  Evidence->Recipe = "fixture-ssa";
  Evidence->EligibleFunctions = Graphs.size();
  for (const auto &G : Graphs) {
    B.beginFunction(G.Function, 0x100, "", NameSource::Generated, 0);
    B.addBlock(G.Function);
    std::set<uint64_t> Addresses;
    for (const auto &N : G.Nodes)
      if (N.OriginSequence >= 0)
        Addresses.insert(N.Address);
    for (auto A : Addresses)
      B.addInstruction(A, {0x90}, "fixture", A, A, InstructionFlow::None);
    Evidence->Entries.push_back(
        {G.Function, FlowAvailability::Available, {}, G});
  }
  auto P = B.finish();
  P.DataFlow = std::move(Evidence);
  return P;
}
DataFlowSettings evidenceSettings() {
  DataFlowSettings S;
  S.Enabled = true;
  return S;
}
FlowRegionSettings regionSettings() {
  FlowRegionSettings S;
  S.Enabled = true;
  return S;
}
} // namespace

TEST(BinaryDiffFlowRegions, SplitMergeAndInlineAreAlternativeTopologyClaims) {
  auto A = program({chains(0x1000, {11, 37})});
  auto B = program({chains(0x2000, {11}), chains(0x3000, {37})});
  B.Instructions[0].Flow = InstructionFlow::Call;
  B.Instructions[0].Callee = 1;
  A.Functions[0].Module = "old-module";
  B.Functions[0].Module = B.Functions[1].Module = "new-module";
  const auto R = findFlowRegions(A, B, evidenceSettings(), regionSettings());
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_EQ(H.PrimaryFunctions, (std::vector<uint64_t>{0x1000}));
  EXPECT_EQ(H.SecondaryFunctions, (std::vector<uint64_t>{0x2000, 0x3000}));
  EXPECT_EQ(H.Explanations,
            (std::vector<RegionKind>{RegionKind::Split, RegionKind::Outline,
                                     RegionKind::ModuleMove}));
  EXPECT_EQ(H.PrimaryNodes, 10u);
  EXPECT_EQ(H.SecondaryNodes, 10u);
  EXPECT_DOUBLE_EQ(H.PrimaryCoverage, 1);
  EXPECT_DOUBLE_EQ(H.SecondaryCoverage, 1);
  EXPECT_TRUE(H.OpenControlFrontier);
  EXPECT_TRUE(H.Ambiguous);
  EXPECT_TRUE(R.PrimaryScopeComplete);
  EXPECT_TRUE(R.SecondaryScopeComplete);
  const auto Again =
      findFlowRegions(A, B, evidenceSettings(), regionSettings());
  EXPECT_EQ(R.Anchors, Again.Anchors);
  EXPECT_EQ(R.Hypotheses, Again.Hypotheses);
  const auto Reverse =
      findFlowRegions(B, A, evidenceSettings(), regionSettings());
  ASSERT_EQ(Reverse.Hypotheses.size(), 1u);
  EXPECT_EQ(Reverse.Hypotheses[0].Explanations,
            (std::vector<RegionKind>{RegionKind::Merge, RegionKind::Inline,
                                     RegionKind::ModuleMove}));
  // The native adapter also admits tail transfers and resolved import slots
  // through Callee. Their spelling must not change the region explanation.
  for (auto Flow : {InstructionFlow::Jump, InstructionFlow::IndirectCall}) {
    B.Instructions[0].Flow = Flow;
    const auto Tail =
        findFlowRegions(A, B, evidenceSettings(), regionSettings());
    ASSERT_EQ(Tail.Hypotheses.size(), 1u);
    EXPECT_EQ(Tail.Hypotheses[0].Explanations, H.Explanations);
  }
}

TEST(BinaryDiffFlowRegions,
     NestedNeighborhoodsAreNotCopiesButMultipleTargetsAre) {
  const auto A = program({chains(0x1000, {11})});
  const auto B = program({chains(0x2000, {11})});
  const auto Shared =
      findFlowRegions(A, B, evidenceSettings(), regionSettings());
  ASSERT_EQ(Shared.Hypotheses.size(), 1u);
  ASSERT_GT(Shared.Anchors.size(), 1u);
  EXPECT_EQ(Shared.Hypotheses[0].Explanations,
            (std::vector<RegionKind>{RegionKind::Shared}));
  EXPECT_FALSE(Shared.Hypotheses[0].Ambiguous);
  EXPECT_EQ(Shared.Hypotheses[0].PrimaryNodes, 5u);
  for (const auto &A : Shared.Anchors) {
    EXPECT_GE(A.PrimaryOrigins, 4u);
    EXPECT_TRUE(A.UniqueSeed);
    EXPECT_TRUE(A.OpenControlFrontier);
  }
  const auto Copies = program({chains(0x2000, {11}), chains(0x3000, {11})});
  const auto R =
      findFlowRegions(A, Copies, evidenceSettings(), regionSettings());
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_EQ(R.Hypotheses[0].Explanations,
            (std::vector<RegionKind>{RegionKind::Split, RegionKind::Clone}));
  EXPECT_EQ(R.Hypotheses[0].PrimaryNodes, 5u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryNodes, 10u);
  for (const auto &A : R.Anchors)
    EXPECT_FALSE(A.UniqueSeed);
}

TEST(BinaryDiffFlowRegions, ConstantsOriginsFrequencyAndCoverageRemainVisible) {
  const auto A = program({chains(0x1000, {11})});
  auto G = chains(0x2000, {12});
  auto R =
      findFlowRegions(A, program({G}), evidenceSettings(), regionSettings());
  EXPECT_TRUE(R.Anchors.empty());
  G = chains(0x2000, {11});
  for (auto &N : G.Nodes)
    N.Address = G.Function;
  R = findFlowRegions(A, program({G}), evidenceSettings(), regionSettings());
  EXPECT_TRUE(R.Anchors.empty());
  EXPECT_GT(R.WeakCandidates, 0u);
  auto S = regionSettings();
  S.MaxOccurrences = 1;
  auto B = program({chains(0x2000, {11}), chains(0x3000, {11})});
  R = findFlowRegions(A, B, evidenceSettings(), S);
  EXPECT_TRUE(R.Anchors.empty());
  EXPECT_GT(R.FrequentSeedGroups, 0u);
  auto Partial = std::make_shared<DataFlowEvidence>(*B.DataFlow);
  Partial->Entries.resize(1);
  Partial->State = ComparisonState::ResourceLimited;
  B.DataFlow = Partial;
  R = findFlowRegions(A, B, evidenceSettings(), regionSettings());
  EXPECT_FALSE(R.SecondaryScopeComplete);
  EXPECT_FALSE(R.Anchors.empty());
  G = chains(0x2000, {11});
  G.Gaps = FlowMemoryAlias;
  auto X = chains(0x1000, {11});
  X.Gaps = FlowMemoryAlias;
  R = findFlowRegions(program({X}), program({G}), evidenceSettings(),
                      regionSettings());
  ASSERT_FALSE(R.Hypotheses.empty());
  EXPECT_NE(R.Hypotheses[0].Gaps & FlowMemoryAlias, 0u);
  EXPECT_TRUE(R.Hypotheses[0].OpenDataFrontier);
}

TEST(BinaryDiffFlowRegions,
     StopsKeepOnlyCompletedEvidenceAndRejectWrongProfiles) {
  const auto A = program({chains(0x1000, {11})});
  const auto B = program({chains(0x2000, {11})});
  for (unsigned Limit = 0; Limit < 4; ++Limit) {
    auto S = regionSettings();
    if (Limit == 0)
      S.MaxSeeds = 1;
    else if (Limit == 1)
      S.MaxWork = 2;
    else if (Limit == 2)
      S.MaxNodePairs = 1;
    else
      S.MaxAnchors = 1;
    const auto R = findFlowRegions(A, B, evidenceSettings(), S);
    EXPECT_EQ(R.State, ComparisonState::ResourceLimited);
    EXPECT_LE(R.Work, S.MaxWork);
    EXPECT_EQ(R.Anchors.size(), Limit == 3 ? 1u : 0u);
    EXPECT_TRUE(R.Hypotheses.empty());
    EXPECT_FALSE(R.StoppedPhase.empty());
  }
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "flow-regions:groups")
      Cancel = true;
  };
  const auto R =
      findFlowRegions(A, B, evidenceSettings(), regionSettings(), Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_FALSE(R.Anchors.empty());
  EXPECT_TRUE(R.Hypotheses.empty());
  EXPECT_THROW(findFlowRegions(A, B, {}, regionSettings()),
               std::invalid_argument);
  auto Bad = B;
  Bad.Identity.ABI = "other-abi";
  EXPECT_THROW(findFlowRegions(A, Bad, evidenceSettings(), regionSettings()),
               std::invalid_argument);
}

TEST(BinaryDiffFlowRegions,
     SavedCorrespondencesRebindAndForgedFactsAreRefused) {
  const auto A = program({chains(0x1000, {11})});
  const auto B = program({chains(0x2000, {11})});
  ComparisonSettings S;
  S.DataFlow = evidenceSettings();
  S.FlowRegions = regionSettings();
  const auto R = compare(A, B, S);
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_FALSE(R.FlowRegions.Anchors.empty());
  std::string Text, Error;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, A, B);
  const auto Saved = readComparison(Text, Error);
  ASSERT_TRUE(Saved) << Error;
  ComparisonResult Bound;
  EXPECT_FALSE(bindComparison(*Saved, A, B, Bound));
  EXPECT_EQ(Bound.FlowRegions, R.FlowRegions);
  auto OtherWork = *Saved;
  ++OtherWork.FlowRegions.Work;
  EXPECT_FALSE(bindComparison(OtherWork, A, B, Bound));
  EXPECT_EQ(Bound.FlowRegions.Work, OtherWork.FlowRegions.Work);
  OtherWork.FlowRegions.Work = S.FlowRegions.MaxWork + 1;
  EXPECT_TRUE(bindComparison(OtherWork, A, B, Bound));
  EXPECT_FALSE(bindComparison(*Saved, A, B, Bound));
  auto Forged = *Saved;
  ++Forged.FlowRegions.Anchors[0].Nodes[0].second;
  BindFailure Why;
  EXPECT_TRUE(bindComparison(Forged, A, B, Bound, &Why));
  EXPECT_EQ(Why, BindFailure::InvalidEvidence);
  EXPECT_EQ(Bound.FlowRegions, R.FlowRegions);
  Forged = *Saved;
  Forged.FlowRegions.Hypotheses[0].PrimaryCoverage = 0.25;
  EXPECT_TRUE(bindComparison(Forged, A, B, Bound));
  Forged = *Saved;
  Forged.FlowRegions = {};
  EXPECT_TRUE(bindComparison(Forged, A, B, Bound));
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "flow-regions:groups")
      Cancel = true;
  };
  auto Partial = *Saved;
  Partial.State = ComparisonState::Cancelled;
  Partial.FlowRegions =
      findFlowRegions(A, B, S.DataFlow, S.FlowRegions, Control);
  Partial.StoppedPhase = Partial.FlowRegions.StoppedPhase;
  EXPECT_FALSE(bindComparison(Partial, A, B, Bound));
  EXPECT_TRUE(Bound.FlowRegions.Hypotheses.empty());
  ++Partial.FlowRegions.Anchors[0].SecondaryRoot;
  EXPECT_TRUE(bindComparison(Partial, A, B, Bound));
  auto JSON = llvm::json::parse(Text);
  ASSERT_TRUE(bool(JSON));
  (*JSON->getAsObject()->getObject("flow_regions"))["semantic_validation"] =
      "equivalent";
  Text.clear();
  OS << *JSON;
  EXPECT_FALSE(readComparison(Text, Error));
  EXPECT_EQ(readFlowRegionSettings(flowRegionSettingsJSON(S.FlowRegions)),
            S.FlowRegions);
  EXPECT_THROW(
      readFlowRegionSettings(llvm::json::Object{{"max_node_pairs", 0}}),
      std::invalid_argument);
}

TEST(BinaryDiffFlowRegions, ManyRootsReuseGraphAdmissionWithinFiniteWork) {
  // Independent source expressions make hundreds of distinct candidate roots
  // in one function. Charging/validating its whole graph per root used to
  // exhaust this allowance before most of the known correspondences survived.
  auto G = chains(0x1000, {});
  for (unsigned C = 0; C < 100; ++C) {
    auto Part = chains(G.Function, {100u * C + 1});
    const auto Begin = uint32_t(G.Nodes.size());
    for (auto N : Part.Nodes) {
      N.Address += Begin;
      N.OriginSequence += Begin;
      for (auto &I : N.Inputs)
        if (I.Type == FlowOperand::Kind::Node)
          I.Node += Begin;
      G.Nodes.push_back(std::move(N));
    }
  }
  G.Blocks[0].End = G.Function + G.Nodes.size();
  auto H = G;
  H.Function += 0x7000;
  H.Blocks[0].Begin += 0x7000;
  H.Blocks[0].End += 0x7000;
  for (auto &N : H.Nodes)
    N.Address += 0x7000;
  auto Settings = regionSettings();
  Settings.MaxWork = 250000;
  const auto R =
      findFlowRegions(program({G}), program({H}), evidenceSettings(), Settings);
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_EQ(R.PrimarySeeds, 500u);
  EXPECT_EQ(R.Anchors.size(), 200u);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_DOUBLE_EQ(R.Hypotheses[0].PrimaryCoverage, 1);
  EXPECT_DOUBLE_EQ(R.Hypotheses[0].SecondaryCoverage, 1);
  EXPECT_LE(R.Work, Settings.MaxWork);
}
