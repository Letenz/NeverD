//===- Trace.cpp - Lifecycle entry and selected module associations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Join retained package, syntax and behavior evidence without executing input.
///
//===----------------------------------------------------------------------===//

#include "../Paths.h"
#include "../behavior/Rules.h"
#include "Internal.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/SourceOrigins.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
namespace {
struct Less {
  uint64_t *Steps;
  bool operator()(std::string_view X, std::string_view Y) const {
    package_install::charge(*Steps, std::min(X.size(), Y.size()) + 1);
    return X < Y;
  }
};
template <class T> using Index = std::map<std::string_view, T, Less>;

class Tracer {
  const PackageAnalysis &P;
  const Snapshot &Namespace;
  const PackageBehavior *Behavior;
  std::span<const PackageInstallInput> Input;
  PackageInstall A;
  Index<const Artifact *> Members{Less{&A.Steps}};
  Index<const PackageFile *> Files{Less{&A.Steps}};
  Index<const PackageBehaviorSource *> Selected{Less{&A.Steps}};
  Index<const SourceAnalysis *> Sources{Less{&A.Steps}};
  Index<std::vector<size_t>> Edges{Less{&A.Steps}};

  void step(uint64_t Count = 1) { package_install::charge(A.Steps, Count); }
  void id(std::string_view ID) {
    step(ID.size() + 1);
    if (ID.size() != 64 || ID.find_first_not_of("0123456789abcdef") != ID.npos)
      throw Error("invalid_package_install_identity");
  }
  std::string package(const PackageFile &F) const {
    return F.Package == NoPackageIndex ? "" : P.Packages.at(F.Package).ID;
  }
  static std::string join(std::string_view Base, std::string_view Path) {
    return Base.empty() ? std::string(Path)
                        : std::string(Base) + '/' + std::string(Path);
  }
  void inventory() {
    if (P.Packages.size() > MaxPackageInstances ||
        P.Files.size() > MaxPackageRecords ||
        P.Scripts.size() > MaxPackageRecords ||
        Namespace.Artifacts.size() > Limits::HardEntries + 1 ||
        Input.size() > MaxPackageBehaviorSources)
      throw Error("package_install_inventory_budget_exceeded");
    id(P.ID);
    id(Namespace.ID);
    if (P.ID !=
        identity("package-analysis", {Namespace.ID, P.ArtifactID, P.BlobHash,
                                      P.Kind, PackageProfile}))
      throw Error("package_install_namespace_mismatch");
    if ((!Behavior && !Input.empty()) ||
        (Behavior && (Behavior->PackageAnalysisID != P.ID ||
                      Behavior->Sources.size() != Input.size())))
      throw Error("package_install_selection_mismatch");
    for (const auto &F : Namespace.Artifacts) {
      step(F.MemberPath.size() + 65);
      if (!Members.emplace(F.MemberPath, &F).second)
        throw Error("invalid_package_install_namespace");
    }
    for (const auto &F : P.Files) {
      id(F.ID);
      id(F.ArtifactID);
      step(F.Path.size());
      if (F.Package != NoPackageIndex && F.Package >= P.Packages.size())
        throw Error("invalid_package_install_inventory");
      const auto Member = Members.find(join(P.RootPrefix, F.Path));
      if (P.DirectoryInventory &&
          (Member == Members.end() || Member->second->ID != F.ArtifactID ||
           Member->second->Directory || Member->second->BlobHash != F.Hash ||
           Member->second->Content.size() != F.Size))
        throw Error("package_install_file_mismatch");
      if (!Files.emplace(F.ArtifactID, &F).second)
        throw Error("invalid_package_install_inventory");
    }
    uint64_t Bytes = 0, Nodes = 0;
    for (const auto &I : Input) {
      if (!I.Source)
        throw Error("invalid_package_install_input");
      const auto &S = *I.Source;
      id(S.ID);
      if (I.SourceBytes > MaxPackageBehaviorBytes - Bytes ||
          S.Nodes.size() > MaxPackageBehaviorNodes - Nodes)
        throw Error("package_install_selection_budget_exceeded");
      Bytes += I.SourceBytes;
      Nodes += S.Nodes.size();
      if (!Sources.emplace(S.ID, &S).second)
        throw Error("duplicate_package_install_source");
    }
    if (!Behavior)
      return;
    id(Behavior->ID);
    uint64_t Records = 0;
    for (const auto &B : Behavior->Sources) {
      id(B.SourceID);
      id(B.BehaviorID);
      const auto S = Sources.find(B.SourceID);
      const auto F = Files.find(B.DeliveryArtifactID);
      if (S == Sources.end() || S->second->ArtifactID != B.ArtifactID ||
          F == Files.end() || F->second->ID != B.FileID ||
          B.BehaviorID != node_behavior::analysisID(B.SourceID) ||
          (B.Status != "partial" && B.Status != "unavailable" &&
           B.Status != "budget_exceeded") ||
          (B.Status != "partial" && !B.Records.empty()) ||
          B.PackageID != package(*F->second) ||
          (B.Relation != "direct_file" && B.Relation != "embedded_source") ||
          (B.Relation == "direct_file" &&
           (B.ArtifactID != B.DeliveryArtifactID ||
            F->second->Hash != S->second->BlobHash)) ||
          !Selected.emplace(B.ArtifactID, &B).second)
        throw Error("package_install_selection_mismatch");
      const auto I =
          std::find_if(Input.begin(), Input.end(), [&](const auto &I) {
            step();
            return I.Source->ID == B.SourceID;
          });
      if (B.Relation == "direct_file" && I->SourceBytes != F->second->Size)
        throw Error("package_install_file_mismatch");
      if (B.Records.size() > MaxPackageBehaviorRecords - Records)
        throw Error("package_install_selection_budget_exceeded");
      Records += B.Records.size();
      for (const auto &R : B.Records)
        id(R.ID);
    }
  }
  void modules() {
    if (!Behavior)
      return;
    // Canonical source ID ordering, independent of caller selection order.
    for (const auto &[SourceID, S] : Sources) {
      step(640);
      const auto &B = *Selected.at(S->ArtifactID);
      PackageInstallSource Row;
      Row.SourceID = S->ID;
      Row.ArtifactID = S->ArtifactID;
      Row.FileID = B.FileID;
      Row.PackageID = B.PackageID;
      Row.BehaviorID = B.BehaviorID;
      A.DirectSources += B.Relation == "direct_file";
      if (B.Relation == "embedded_source") {
        ++A.EmbeddedSources;
        Row.Status = "embedded_source_boundary";
      } else if (S->ParseStatus != "parsed" || B.Status != "partial") {
        ++A.UnavailableSources;
        Row.Status = "source_unavailable";
      } else {
        const auto Bindings = analyzeSourceBindings(*S);
        const auto M = analyzeSourceModules(*S, Bindings);
        const auto O = analyzeSourceOrigins(*S, Bindings, M);
        const auto L = linkNodeSourceModules(*S, M, O, Namespace);
        step(Bindings.Steps);
        step(M.Steps);
        step(O.Steps);
        step(L.Steps);
        if (Bindings.Status == "budget_exceeded" ||
            M.Status == "budget_exceeded" || O.Status == "budget_exceeded" ||
            L.Status == "budget_exceeded")
          throw Error("package_install_source_budget_exceeded");
        Row.ModuleID = M.ID;
        Row.OriginID = O.ID;
        Row.LinkID = L.ID;
        Row.Status = L.Status == "ok" ? "direct_source_selected"
                                      : "module_evidence_unavailable";
        A.UnavailableSources += L.Status != "ok";
        if (L.Status == "ok") {
          if (L.Requests.size() != M.Requests.size())
            throw Error("invalid_package_install_module_links");
          for (size_t I = 0; I < M.Requests.size(); ++I) {
            step(1024);
            if (A.Requests.size() >= MaxPackageInstallRequests)
              throw Error("package_install_request_budget_exceeded");
            const auto &R = M.Requests[I];
            const auto &Link = L.Requests[I];
            PackageInstallRequest Q;
            Q.SourceID = S->ID;
            Q.RequestID = R.ID;
            Q.NodeID = S->Nodes.at(R.Node).ID;
            Q.Kind = R.Kind;
            Q.OriginEvidence = Link.OriginEvidence;
            if (R.Kind == "esm_import" || R.Kind == "esm_reexport")
              Q.OriginEvidence = "esm_declaration_syntax";
            Q.Status = Link.Status == "not_in_snapshot"
                           ? "not_captured_or_unavailable"
                           : Link.Status;
            Q.ArtifactID = Link.ArtifactID;
            if (!Q.ArtifactID.empty()) {
              if (const auto F = Files.find(Q.ArtifactID); F == Files.end()) {
                Q.Status = "outside_package_inventory";
              } else {
                Q.FileID = F->second->ID;
                Q.PackageID = package(*F->second);
                const auto Target = Selected.find(Q.ArtifactID);
                if (Target == Selected.end())
                  Q.Status = "target_source_unselected";
                else if (Target->second->Relation != "direct_file")
                  Q.Status = "embedded_source_boundary";
                else {
                  Q.TargetSourceID = Target->second->SourceID;
                  Q.Status = Target->second->Status == "partial"
                                 ? "selected_file_candidate"
                                 : "target_source_unavailable";
                }
              }
            }
            Q.ID = identity("package-install-request",
                            {A.ID, L.ID, R.ID, Q.Status, Q.TargetSourceID});
            if (Q.Status == "selected_file_candidate")
              Edges[S->ID].push_back(A.Requests.size());
            else
              ++A.UnresolvedRequests;
            A.Requests.push_back(std::move(Q));
          }
        }
      }
      A.Sources.push_back(std::move(Row));
    }
    for (auto &[ID, E] : Edges)
      std::sort(E.begin(), E.end(), [&](size_t X, size_t Y) {
        step(64);
        return A.Requests[X].ID < A.Requests[Y].ID;
      });
  }
  void scripts() {
    for (const auto &S : P.Scripts) {
      step(S.Kind.size() + 1);
      if (S.Kind != "lifecycle_declaration")
        continue;
      if (A.Scripts.size() >= MaxPackageInstallScripts)
        throw Error("package_install_script_budget_exceeded");
      if (S.Package >= P.Packages.size())
        throw Error("invalid_package_install_script");
      const auto &Owner = P.Packages[S.Package];
      step(640);
      PackageInstallScript R;
      R.ScriptID = S.ID;
      R.PackageID = Owner.ID;
      R.ManifestID = S.ArtifactID;
      R.Kind = "lifecycle_declaration";
      ++A.LifecycleDeclarations;
      const auto C = package_install::command(S.Command, A.Steps);
      R.CommandStatus = C.Status;
      R.Status = C.Status;
      if (C.Status == "literal_node_script_candidate") {
        R.CommandStart = C.Start;
        R.CommandEnd = C.End;
        auto Path = C.Path;
        if (Path.starts_with("./"))
          Path.remove_prefix(2);
        step(Path.size() + Owner.Location.size() + P.RootPrefix.size());
        if (Owner.ManifestStatus == "conflicting_declarations")
          R.Status = "conflicting_manifest";
        else if (Owner.Link)
          R.Status = "package_link_boundary";
        else if (!package_paths::confined(Path))
          R.Status = "unsupported_script_path";
        else if (!P.DirectoryInventory)
          R.Status = "no_directory_inventory";
        else {
          const auto Target =
              Members.find(join(P.RootPrefix, join(Owner.Location, Path)));
          if (Target == Members.end())
            R.Status = "not_captured_or_unavailable";
          else if (Target->second->Directory)
            R.Status = "directory_target";
          else {
            R.ArtifactID = Target->second->ID;
            const auto F = Files.find(R.ArtifactID);
            if (F == Files.end())
              throw Error("invalid_package_install_inventory");
            R.FileID = F->second->ID;
            const auto Found = Selected.find(R.ArtifactID);
            if (Found == Selected.end())
              R.Status = "entry_source_unselected";
            else if (Found->second->Relation != "direct_file")
              R.Status = "embedded_source_boundary";
            else {
              R.SourceID = Found->second->SourceID;
              R.Status = Found->second->Status == "partial"
                             ? "selected_entry_candidate"
                             : "entry_source_unavailable";
            }
          }
        }
      }
      R.ID = identity("package-install-script", {A.ID, S.ID, R.Status});
      A.Scripts.push_back(std::move(R));
    }
    for (const auto &Owner : P.Packages) {
      step();
      if (!Owner.HasInstallScript || !Owner.ManifestArtifactID.empty())
        continue;
      if (A.Scripts.size() >= MaxPackageInstallScripts)
        throw Error("package_install_script_budget_exceeded");
      PackageInstallScript R;
      step(256);
      R.ID = identity("package-install-presence", {A.ID, Owner.ID});
      R.PackageID = Owner.ID;
      R.Kind = "install_presence_declaration";
      R.Status = "manifest_not_supplied";
      R.CommandStatus = "command_not_supplied";
      ++A.PresenceDeclarations;
      A.Scripts.push_back(std::move(R));
    }
  }
  void associate() {
    for (const auto &Script : A.Scripts) {
      if (Script.Status != "selected_entry_candidate")
        continue;
      struct Visit {
        std::string_view SourceID;
        std::vector<std::string> Path;
      };
      std::vector<Visit> Queue{{Script.SourceID, {}}};
      std::set<std::string_view, Less> Seen(Less{&A.Steps});
      Seen.insert(Script.SourceID);
      for (size_t I = 0; I < Queue.size(); ++I) {
        const auto Current = Queue[I];
        step(64 * (Current.Path.size() + 1));
        const auto &S = *Sources.at(Current.SourceID);
        const auto &B = *Selected.at(S.ArtifactID);
        for (const auto &Record : B.Records) {
          step(768 + 64 * Current.Path.size());
          if (A.Associations.size() >= MaxPackageInstallAssociations)
            throw Error("package_install_association_budget_exceeded");
          PackageInstallAssociation R;
          R.ScriptTraceID = Script.ID;
          R.ScriptID = Script.ScriptID;
          R.EntryFileID = Script.FileID;
          R.SourceID = S.ID;
          R.BehaviorID = B.BehaviorID;
          R.RecordID = Record.ID;
          R.NodeID = Record.NodeID;
          R.API = Record.API;
          R.Kind = Record.Kind;
          R.OriginEvidence = Record.OriginEvidence;
          R.Start = Record.Start;
          R.End = Record.End;
          R.FileID = B.FileID;
          R.PackageID = B.PackageID;
          R.RequestIDs = Current.Path;
          std::string PathIDs;
          for (const auto &ID : Current.Path)
            PathIDs += ID;
          R.ID = identity("package-install-association",
                          {A.ID, Script.ID, Record.ID, PathIDs});
          A.Associations.push_back(std::move(R));
        }
        const auto E = Edges.find(Current.SourceID);
        if (E == Edges.end())
          continue;
        for (const auto Index : E->second) {
          step(64);
          const auto &R = A.Requests[Index];
          if (!Seen.insert(R.TargetSourceID).second)
            continue;
          if (Current.Path.size() >= MaxPackageInstallDepth)
            throw Error("package_install_depth_budget_exceeded");
          Visit Next{R.TargetSourceID, Current.Path};
          step(64 * (Current.Path.size() + 1));
          Next.Path.push_back(R.ID);
          Queue.push_back(std::move(Next));
        }
      }
    }
  }

public:
  Tracer(const PackageAnalysis &P, const Snapshot &N, const PackageBehavior *B,
         std::span<const PackageInstallInput> I)
      : P(P), Namespace(N), Behavior(B), Input(I) {}
  PackageInstall run() {
    inventory();
    A.PackageAnalysisID = P.ID;
    A.NamespaceID = Namespace.ID;
    A.PackageBehaviorID = Behavior ? Behavior->ID : "";
    // Selected source IDs are already bound transitively by package behavior.
    // Bind every shared semantic profile too: changes to an origin or link
    // owner must not reuse a receipt for a different candidate graph.
    A.ID = identity("package-install",
                    {PackageInstallProfile, P.ID, Namespace.ID,
                     A.PackageBehaviorID, JavaScriptBindingProfile,
                     JavaScriptModuleProfile, JavaScriptOriginProfile,
                     NodeModuleLinkProfile});
    modules();
    scripts();
    associate();
    return std::move(A);
  }
};
} // namespace

PackageInstall analyzePackageInstall(const PackageAnalysis &P,
                                     const Snapshot &N,
                                     const PackageBehavior *B,
                                     std::span<const PackageInstallInput> I) {
  return Tracer(P, N, B, I).run();
}
} // namespace neverd::web
