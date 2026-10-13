//===- Diff.cpp - Bounded selected package behavior comparison ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Join exact file candidates and count existing syntax records per source
/// pair.
///
//===----------------------------------------------------------------------===//

#include "../Paths.h"
#include "Rules.h"

#include "neverd/web/Error.h"
#include "neverd/web/PackageBehaviorDiff.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
namespace {
void charge(uint64_t &Steps, uint64_t N = 1) {
  if (N > MaxPackageBehaviorDiffSteps - Steps)
    throw Error("package_behavior_diff_work_budget_exceeded");
  Steps += N;
}
struct Less {
  uint64_t *Steps;
  bool operator()(std::string_view X, std::string_view Y) const {
    charge(*Steps, std::min(X.size(), Y.size()) + 1);
    return X < Y;
  }
};
template <class T> using Index = std::map<std::string_view, T, Less>;

struct Side {
  const PackageAnalysis &P;
  const PackageBehavior *B;
  Index<const PackageFile *> Paths, Artifacts;
  Index<const PackageBehaviorSource *> Selected;
  Side(const PackageAnalysis &P, const PackageBehavior *B, uint64_t &Steps)
      : P(P), B(B), Paths(Less{&Steps}), Artifacts(Less{&Steps}),
        Selected(Less{&Steps}) {}
};

class Comparison {
  const PackageDiff &Base;
  PackageBehaviorDiff D;
  Side Before, After;

  void step(uint64_t N = 1) { charge(D.Steps, N); }
  void id(std::string_view ID) {
    step(ID.size() + 1);
    if (ID.size() != 64 || ID.find_first_not_of("0123456789abcdef") != ID.npos)
      throw Error("invalid_package_behavior_diff_identity");
  }
  std::string package(const Side &S, const PackageFile &F) const {
    return F.Package == NoPackageIndex ? "" : S.P.Packages.at(F.Package).ID;
  }
  void inventory(Side &S) {
    const auto &P = S.P;
    id(P.ID);
    if (P.Packages.empty() || P.Packages.size() > MaxPackageInstances ||
        P.Files.size() > MaxPackageRecords)
      throw Error("package_behavior_diff_inventory_budget_exceeded");
    for (const auto &F : P.Files) {
      step(F.Path.size() + 1);
      id(F.ID);
      id(F.ArtifactID);
      id(F.Hash);
      if ((P.DirectoryInventory && !package_paths::confined(F.Path)) ||
          (F.Package != NoPackageIndex && F.Package >= P.Packages.size()) ||
          !S.Paths.emplace(F.Path, &F).second ||
          !S.Artifacts.emplace(F.ArtifactID, &F).second)
        throw Error("invalid_package_behavior_diff_inventory");
    }
    if (!S.B)
      return;
    id(S.B->ID);
    if (S.B->PackageAnalysisID != P.ID)
      throw Error("package_behavior_diff_selection_mismatch");
    if (S.B->Sources.empty() || S.B->Sources.size() > MaxPackageBehaviorSources)
      throw Error("package_behavior_diff_source_budget_exceeded");
    uint64_t Records = 0, Bytes = 0;
    for (const auto &B : S.B->Sources) {
      id(B.SourceID);
      id(B.ArtifactID);
      id(B.BehaviorID);
      if (B.BehaviorID != node_behavior::analysisID(B.SourceID) ||
          (B.SourceType != "script" && B.SourceType != "module" &&
           B.SourceType != "commonjs") ||
          (B.Status != "partial" && B.Status != "unavailable" &&
           B.Status != "budget_exceeded") ||
          (B.Status != "partial" && !B.Records.empty()) ||
          (B.Relation != "direct_file" && B.Relation != "embedded_source") ||
          !S.Selected.emplace(B.ArtifactID, &B).second)
        throw Error("invalid_package_behavior_diff_source");
      const auto F = S.Artifacts.find(B.DeliveryArtifactID);
      if (F == S.Artifacts.end() || F->second->ID != B.FileID ||
          package(S, *F->second) != B.PackageID)
        throw Error("package_behavior_diff_file_mismatch");
      if (B.Relation == "direct_file" &&
          (B.ArtifactID != B.DeliveryArtifactID || !B.OriginIDs.empty() ||
           B.SourceHash != F->second->Hash || B.SourceBytes != F->second->Size))
        throw Error("package_behavior_diff_file_mismatch");
      if (B.Relation == "embedded_source" &&
          (B.ArtifactID == B.DeliveryArtifactID || B.OriginIDs.empty()))
        throw Error("package_behavior_diff_file_mismatch");
      if (B.Records.size() > MaxPackageBehaviorRecords - Records ||
          B.SourceBytes > MaxPackageBehaviorBytes - Bytes)
        throw Error("package_behavior_diff_selection_budget_exceeded");
      Records += B.Records.size();
      Bytes += B.SourceBytes;
      std::set<std::string_view, Less> Seen(Less{&D.Steps});
      for (const auto &R : B.Records) {
        step(256 + R.API.size() + R.Kind.size() + R.OriginEvidence.size() +
             R.Encoding.size());
        id(R.ID);
        id(R.NodeID);
        if (R.Start > R.End || R.End > B.SourceBytes ||
            !Seen.insert(R.ID).second ||
            R.ID != identity("node-behavior-record",
                             {B.BehaviorID, R.NodeID, R.API, R.Kind}))
          throw Error("invalid_package_behavior_diff_record");
      }
    }
    if (Records != S.B->RecordCount)
      throw Error("invalid_package_behavior_diff_record_count");
  }
  PackageBehaviorDiffEndpoint endpoint(const Side &S, const PackageFile *F,
                                       const PackageBehaviorSource *B) {
    PackageBehaviorDiffEndpoint R;
    if (!F) {
      R.Status = "not_captured_or_unavailable";
      return R;
    }
    step(384);
    R.FileID = F->ID;
    R.ArtifactID = F->ArtifactID;
    R.PackageID = package(S, *F);
    if (!B) {
      R.Status = S.B ? "source_unselected" : "behavior_selection_not_supplied";
      return R;
    }
    R.SourceID = B->SourceID;
    R.ArtifactID = B->ArtifactID;
    R.BehaviorID = B->BehaviorID;
    R.SourceType = B->SourceType;
    R.BehaviorStatus = B->Status;
    R.OriginStatus = B->OriginStatus;
    R.ValueStatus = B->ValueStatus;
    if (B->Relation != "direct_file") {
      R.Status = "embedded_source_boundary";
      R.PackageID.clear();
    } else if (B->Status != "partial")
      R.Status = "behavior_unavailable";
    else if (B->OriginStatus != "partial" ||
             (B->ValueStatus != "ok" && B->ValueStatus != "partial"))
      R.Status = "upstream_coverage_unavailable";
    else
      R.Status = "selected_syntax_evidence";
    return R;
  }
  void patterns(const PackageBehaviorSourcePair &Pair,
                const PackageBehaviorSource &B,
                const PackageBehaviorSource &A) {
    std::map<std::string, size_t, Less> Groups(Less{&D.Steps});
    auto Add = [&](const PackageBehaviorSource &S, bool IsAfter) {
      for (const auto &R : S.Records) {
        step(768);
        const auto ID = identity(
            "package-behavior-pattern",
            {D.ID, Pair.ID, R.API, R.Kind, R.OriginEvidence, R.Encoding,
             std::to_string(R.ArgumentCount), R.Optional ? "optional" : "plain",
             R.Spread ? "spread" : "plain"});
        auto [It, New] = Groups.emplace(ID, D.Patterns.size());
        if (New) {
          if (D.Patterns.size() >= MaxPackageBehaviorDiffOccurrences)
            throw Error("package_behavior_diff_pattern_budget_exceeded");
          PackageBehaviorPattern G;
          G.ID = ID;
          G.PairID = Pair.ID;
          G.API = R.API;
          G.Kind = R.Kind;
          G.OriginEvidence = R.OriginEvidence;
          G.Encoding = R.Encoding;
          G.ArgumentCount = R.ArgumentCount;
          G.Optional = R.Optional;
          G.Spread = R.Spread;
          D.Patterns.push_back(std::move(G));
        }
        auto &G = D.Patterns[It->second];
        ++(IsAfter ? G.After : G.Before);
        if (D.Occurrences.size() >= MaxPackageBehaviorDiffOccurrences)
          throw Error("package_behavior_diff_occurrence_budget_exceeded");
        PackageBehaviorOccurrence O;
        O.Side = IsAfter ? "after" : "before";
        O.PackageBehaviorID = IsAfter ? D.AfterBehaviorID : D.BeforeBehaviorID;
        O.PairID = Pair.ID;
        O.GroupID = G.ID;
        O.SourceID = S.SourceID;
        O.ArtifactID = S.ArtifactID;
        O.FileID = S.FileID;
        O.BehaviorID = S.BehaviorID;
        O.RecordID = R.ID;
        O.NodeID = R.NodeID;
        O.Start = R.Start;
        O.End = R.End;
        O.ID = identity("package-behavior-occurrence",
                        {D.ID, G.ID, O.Side, S.SourceID, R.ID});
        D.Occurrences.push_back(std::move(O));
      }
    };
    Add(B, false);
    Add(A, true);
  }
  void pair(const PackageFile *BF, const PackageFile *AF,
            const PackageBehaviorSource *B, const PackageBehaviorSource *A) {
    if (D.Pairs.size() >= MaxPackageBehaviorDiffPairs)
      throw Error("package_behavior_diff_pair_budget_exceeded");
    PackageBehaviorSourcePair P;
    P.Before = endpoint(Before, BF, B);
    P.After = endpoint(After, AF, A);
    P.Bytes = !BF || !AF             ? "counterpart_not_captured"
              : BF->Hash == AF->Hash ? "same_supplied_file_bytes"
                                     : "different_supplied_file_bytes";
    if ((B && B->Relation != "direct_file") ||
        (A && A->Relation != "direct_file")) {
      P.Status = "embedded_source_boundary";
      P.Bytes = "embedded_sources_not_paired";
    } else if (!Before.P.DirectoryInventory || !After.P.DirectoryInventory)
      P.Status = "inventory_coverage_unavailable";
    else if (Base.RootIdentity == "different_declared_name" ||
             Base.Platform != "same_root_conditions_host_not_selected" ||
             Base.InputContract != "same_metadata_contract")
      P.Status = "incompatible_package_context";
    else if (P.Before.Status != "selected_syntax_evidence" ||
             P.After.Status != "selected_syntax_evidence")
      P.Status = "source_coverage_unavailable";
    else if (P.Before.SourceType != P.After.SourceType)
      P.Status = "source_type_mismatch";
    else
      P.Status = "compared_recognized_patterns";
    P.ID = identity("package-behavior-source-pair",
                    {D.ID, P.Before.FileID, P.After.FileID, P.Before.SourceID,
                     P.After.SourceID, P.Status});
    if (P.Status == "compared_recognized_patterns") {
      ++D.ComparedPairs;
      patterns(P, *B, *A);
    }
    D.Pairs.push_back(std::move(P));
  }
  void sources() {
    std::set<std::string_view, Less> Paths(Less{&D.Steps});
    for (const auto *S : {&Before, &After})
      for (const auto &[Artifact, B] : S->Selected) {
        const auto *F = S->Artifacts.at(B->DeliveryArtifactID);
        if (B->Relation == "direct_file")
          Paths.insert(F->Path);
        else if (S == &Before)
          pair(F, nullptr, B, nullptr);
        else
          pair(nullptr, F, nullptr, B);
      }
    for (const auto Path : Paths) {
      auto Find = [&](const Side &S) {
        const auto F = S.Paths.find(Path);
        const PackageFile *File = F == S.Paths.end() ? nullptr : F->second;
        const auto B =
            File ? S.Selected.find(File->ArtifactID) : S.Selected.end();
        const PackageBehaviorSource *Behavior =
            B == S.Selected.end() ? nullptr : B->second;
        return std::pair(File, Behavior);
      };
      const auto [BF, B] = Find(Before);
      const auto [AF, A] = Find(After);
      pair(BF, AF, B, A);
    }
  }

public:
  Comparison(const PackageDiff &Base, const PackageAnalysis &B,
             const PackageAnalysis &A, const PackageBehavior *BB,
             const PackageBehavior *AB)
      : Base(Base), Before(B, BB, D.Steps), After(A, AB, D.Steps) {}
  PackageBehaviorDiff run() {
    if (Base.BeforeID != Before.P.ID || Base.AfterID != After.P.ID ||
        Base.ID !=
            identity("package-diff", {Before.P.ID, After.P.ID, PackageProfile}))
      throw Error("package_behavior_diff_base_mismatch");
    inventory(Before);
    inventory(After);
    D.PackageDiffID = Base.ID;
    D.BeforeID = Base.BeforeID;
    D.AfterID = Base.AfterID;
    D.BeforeBehaviorID = Before.B ? Before.B->ID : "";
    D.AfterBehaviorID = After.B ? After.B->ID : "";
    D.RootIdentity = Base.RootIdentity;
    D.Coverage = Base.Coverage;
    D.Platform = Base.Platform;
    D.InputContract = Base.InputContract;
    D.ID = identity("package-behavior-diff",
                    {PackageBehaviorDiffProfile, Base.ID, D.BeforeBehaviorID,
                     D.AfterBehaviorID, NodeBehaviorProfile});
    sources();
    auto Sort = [&](auto &Rows) {
      std::sort(Rows.begin(), Rows.end(), [&](const auto &X, const auto &Y) {
        step(64);
        return X.ID < Y.ID;
      });
    };
    Sort(D.Pairs);
    Sort(D.Patterns);
    Sort(D.Occurrences);
    return std::move(D);
  }
};
} // namespace

PackageBehaviorDiff comparePackageBehavior(const PackageDiff &Base,
                                           const PackageAnalysis &B,
                                           const PackageAnalysis &A,
                                           const PackageBehavior *BB,
                                           const PackageBehavior *AB) {
  return Comparison(Base, B, A, BB, AB).run();
}
} // namespace neverd::web
