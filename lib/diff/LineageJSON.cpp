//===- LineageJSON.cpp - Revision-bound evidence pages --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared projection policy for CLI, SDK and desktop. Paging never promotes
/// ambiguous hypotheses or manual patch assessments into accepted graph edges.
//===----------------------------------------------------------------------===//

#include "neverd/diff/LineageJSON.h"

#include "neverd/diff/FlowRegionsJSON.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <utility>

namespace neverd::diff {
namespace {
namespace limits = ::neverd::limits::diff;

void require(bool Condition, llvm::StringRef Message) {
  if (!Condition)
    throw std::invalid_argument(Message.str());
}

void fields(const llvm::json::Object &O,
            std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &[Name, Value] : O) {
    (void)Value;
    require(llvm::is_contained(Allowed, llvm::StringRef(Name)),
            "unknown lineage option: " + Name.str());
  }
}

std::string text(const llvm::json::Object &O, llvm::StringRef Key,
                 bool Required = false) {
  const auto *V = O.get(Key);
  if (!V) {
    require(!Required, "missing lineage option: " + Key.str());
    return {};
  }
  const auto S = V->getAsString();
  require(S.has_value() && (!Required || !S->empty()),
          "lineage option must be a nonempty string: " + Key.str());
  return S->str();
}

uint64_t count(const llvm::json::Object &O, llvm::StringRef Key,
               uint64_t Default, uint64_t Maximum, bool Positive = false) {
  const auto *V = O.get(Key);
  if (!V)
    return Default;
  const auto N = V->getAsInteger();
  require(N && *N >= (Positive ? 1 : 0) && uint64_t(*N) <= Maximum,
          "lineage option is outside its integer range: " + Key.str());
  return uint64_t(*N);
}

std::string hex(uint64_t Address) {
  return "0x" + llvm::utohexstr(Address, true);
}

llvm::json::Value index(uint32_t Index) {
  return Index == NoIndex ? llvm::json::Value(nullptr)
                          : llvm::json::Value(Index);
}

bool incomplete(const SavedComparison &R) {
  return R.State != ComparisonState::Complete ||
         !R.PrimaryCoverage.DiscoveryComplete ||
         !R.SecondaryCoverage.DiscoveryComplete ||
         R.PrimaryCoverage.Incomplete || R.SecondaryCoverage.Incomplete;
}

llvm::json::Object function(const Lineage &L, uint32_t N) {
  const auto &Node = L.nodes()[N];
  const auto &V = L.versions()[Node.Version];
  const auto &F = L.functions(Node.Version)[Node.Function];
  return llvm::json::Object{{"node", N},
                            {"version", V.ID},
                            {"version_label", V.Label},
                            {"address", hex(F.Address)},
                            {"name", F.Name},
                            {"size", F.Size},
                            {"traits", F.Traits},
                            {"blocks", F.Blocks},
                            {"instructions", F.Instructions},
                            {"component", Node.Component}};
}

llvm::json::Object edge(const Lineage &L, uint32_t I) {
  const auto &E = L.edges()[I];
  const auto &R = L.reports()[E.Report];
  const auto &M = R.Evidence->Saved.Matches[E.Match].Match;
  llvm::json::Array Changes;
  for (const auto C : changeNames(M.Changes))
    Changes.push_back(C);
  return llvm::json::Object{{"edge", I},
                            {"primary", E.Primary},
                            {"secondary", E.Secondary},
                            {"report", R.Evidence->ID},
                            {"match", E.Match},
                            {"step", stepName(M.Step)},
                            {"similarity", M.Similarity},
                            {"confidence", M.Confidence},
                            {"confirmed", M.Confirmed},
                            {"measured", M.Measured},
                            {"identity_only", M.IdentityOnly},
                            {"analysis_complete", M.AnalysisComplete},
                            {"changes", std::move(Changes)}};
}

llvm::json::Object conflict(const Lineage &L, uint32_t I) {
  const auto &C = L.conflicts()[I];
  return llvm::json::Object{
      {"conflict", I},
      {"kind", C.Kind == LineageConflictKind::MultipleFunctions
                   ? "multiple_functions"
                   : "rejected_pair"},
      {"component", C.Component},
      {"version", C.Version == NoIndex
                      ? llvm::json::Value(nullptr)
                      : llvm::json::Value(L.versions()[C.Version].ID)},
      {"version_label", C.Version == NoIndex
                            ? llvm::json::Value(nullptr)
                            : llvm::json::Value(L.versions()[C.Version].Label)},
      {"report", C.Report == NoIndex
                     ? llvm::json::Value(nullptr)
                     : llvm::json::Value(L.reports()[C.Report].Evidence->ID)},
      {"members", C.Members.size()}};
}

struct PageBuilder {
  const LineageQuery &Query;
  const ComparisonControl &Control;
  llvm::json::Array Rows;
  uint64_t Total = 0, Work = 0, Bytes = 0;

  void charge(uint64_t Amount = 1) {
    Control.check();
    if (Amount > Query.MaxWork - Work)
      throw ComparisonInterrupted(ComparisonState::ResourceLimited);
    Work += Amount;
  }

  template <typename Render> void append(Render Make) {
    charge();
    const auto Position = Total++;
    if (Position < Query.Offset || Rows.size() >= Query.Limit)
      return;
    llvm::json::Value Row = Make();
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    OS << Row;
    charge(Text.size());
    if (Text.size() > limits::kMaxLineagePageBytes - Bytes)
      throw ComparisonInterrupted(ComparisonState::ResourceLimited);
    Bytes += Text.size();
    Rows.push_back(std::move(Row));
  }
};
} // namespace

LineageSettings readLineageSettings(const llvm::json::Object &O) {
  fields(O, {"max_source_bytes", "max_nodes", "max_edges", "max_work",
             "timeout_ms"});
  LineageSettings S;
  S.MaxSourceBytes = count(O, "max_source_bytes", S.MaxSourceBytes,
                           limits::kMaxSavedComparisonBytes, true);
  S.MaxNodes = count(O, "max_nodes", S.MaxNodes, NoIndex - 1, true);
  S.MaxEdges = count(O, "max_edges", S.MaxEdges, NoIndex - 1, true);
  S.MaxWork = count(O, "max_work", S.MaxWork, INT64_MAX, true);
  (void)count(O, "timeout_ms", 0, limits::kMaxTimeoutMilliseconds);
  return S;
}

LineageQuery readLineageQuery(const llvm::json::Object &O) {
  fields(O, {"kind", "revision", "version", "address", "report", "search",
             "index", "offset", "limit", "max_work", "timeout_ms", "latest"});
  LineageQuery Q;
  Q.Kind = text(O, "kind", true);
  Q.Revision = text(O, "revision", true);
  Q.Version = text(O, "version");
  Q.Report = text(O, "report");
  Q.Search = text(O, "search");
  Q.Offset = count(O, "offset", 0, INT64_MAX);
  Q.Limit = count(O, "limit", Q.Limit, limits::kMaxPageSize, true);
  Q.Index = count(O, "index", 0, NoIndex - 1);
  Q.MaxWork = count(O, "max_work", Q.MaxWork, INT64_MAX, true);
  Q.TimeoutMilliseconds =
      count(O, "timeout_ms", 0, limits::kMaxTimeoutMilliseconds);
  if (const auto *V = O.get("latest")) {
    const auto B = V->getAsBoolean();
    require(B.has_value(), "latest must be a boolean");
    Q.Latest = *B;
  }
  const bool Trace = llvm::StringRef(Q.Kind).starts_with("trace_");
  require(llvm::is_contained(
              std::initializer_list<llvm::StringRef>{
                  "versions", "reports", "functions", "edges", "conflicts",
                  "conflict_members", "trace_nodes", "trace_edges",
                  "trace_conflicts", "reviews", "decisions", "ambiguities",
                  "ambiguity_primary", "ambiguity_secondary", "warnings",
                  "native_regions", "native_region_anchors", "flow_regions",
                  "flow_region_anchors"},
              llvm::StringRef(Q.Kind)),
          "unknown lineage query kind");
  require((Trace || Q.Kind == "functions") == !Q.Version.empty(),
          "version is required only for functions and trace queries");
  require(Trace == (O.get("address") != nullptr),
          "address is required only for trace queries");
  if (Trace) {
    const auto Address = text(O, "address", true);
    require(!llvm::StringRef(Address).getAsInteger(0, Q.Address),
            "trace address must be an unsigned address string");
  }
  const bool ReportQuery =
      Q.Kind == "decisions" || Q.Kind == "ambiguities" ||
      Q.Kind == "ambiguity_primary" || Q.Kind == "ambiguity_secondary" ||
      Q.Kind == "warnings" || Q.Kind == "native_regions" ||
      Q.Kind == "native_region_anchors" || Q.Kind == "flow_regions" ||
      Q.Kind == "flow_region_anchors";
  require(!ReportQuery || !Q.Report.empty(), "source report is required");
  require(ReportQuery || Q.Kind == "reviews" || Q.Report.empty(),
          "report is not used by this query kind");
  const bool Indexed =
      Q.Kind == "conflict_members" || Q.Kind == "ambiguity_primary" ||
      Q.Kind == "ambiguity_secondary" || Q.Kind == "native_region_anchors" ||
      Q.Kind == "flow_region_anchors";
  require(Indexed == (O.get("index") != nullptr),
          "index is required only for conflict, ambiguity or region members");
  require(Q.Kind == "functions" || !O.get("search"),
          "search is only valid for functions");
  require(Q.Kind == "reviews" || !O.get("latest"),
          "latest is only valid for reviews");
  return Q;
}

llvm::json::Object lineageSummary(const Lineage &L,
                                  const ComparisonControl &Control) {
  uint64_t Incomplete = 0, Reviews = 0, Ambiguities = 0, Regions = 0;
  for (const auto &R : L.reports()) {
    Control.check();
    const auto &S = R.Evidence->Saved;
    Incomplete += incomplete(S);
    Reviews += S.Reviews.Records.size();
    Ambiguities += S.Ambiguities.size();
    Regions += S.Regions.Hypotheses.size() + S.FlowRegions.Hypotheses.size();
  }
  return llvm::json::Object{{"schema", "neverd.bindiff.lineage.summary"},
                            {"schema_version", LineageSchemaVersion},
                            {"revision", L.revision()},
                            {"provenance", "retained_reports"},
                            {"native_authenticated", false},
                            {"versions", L.versions().size()},
                            {"reports", L.reports().size()},
                            {"nodes", L.nodes().size()},
                            {"edges", L.edges().size()},
                            {"conflicts", L.conflicts().size()},
                            {"incomplete_reports", Incomplete},
                            {"reviews", Reviews},
                            {"ambiguities", Ambiguities},
                            {"region_hypotheses", Regions},
                            {"source_bytes", L.sourceBytes()},
                            {"admission_work", L.work()}};
}

LineagePage lineagePage(const Lineage &L, const LineageQuery &Q,
                        const ComparisonControl &Control,
                        std::shared_ptr<const LineageTrace> Cache) {
  require(Q.Revision == L.revision(), "stale lineage revision");
  require(Q.Limit && Q.Limit <= limits::kMaxPageSize && Q.MaxWork,
          "invalid lineage query allowance");
  PageBuilder B{Q, Control, {}};
  LineagePage Result;
  Control.report("lineage:query");
  if (llvm::StringRef(Q.Kind).starts_with("trace_")) {
    const auto V = L.version(Q.Version);
    const auto N = V ? L.node(*V, Q.Address) : std::nullopt;
    require(N.has_value(), "trace function is absent from this snapshot");
    if (!Cache || Cache->revision() != L.revision() || Cache->source() != *N)
      Cache = std::make_shared<LineageTrace>(L.trace(*N, Q.MaxWork, Control));
    B.charge(Cache->work());
    Result.Trace = Cache;
    if (Q.Kind == "trace_nodes")
      for (const auto &N : Cache->nodes())
        B.append([&] {
          auto Row = function(L, N.Node);
          Row["distance"] = N.Distance;
          Row["predecessor"] = index(N.Predecessor);
          Row["witness_edge"] = index(N.Edge);
          return Row;
        });
    else if (Q.Kind == "trace_edges")
      for (const auto E : Cache->edges())
        B.append([&] { return edge(L, E); });
    else if (Q.Kind == "trace_conflicts")
      for (const auto C : Cache->conflicts())
        B.append([&] { return conflict(L, C); });
    else
      throw std::invalid_argument("unknown lineage trace kind");
  } else if (Q.Kind == "versions") {
    for (uint32_t I = 0; I < L.versions().size(); ++I)
      B.append([&] {
        const auto &V = L.versions()[I];
        return llvm::json::Object{
            {"id", V.ID},
            {"label", V.Label},
            {"branch", V.Branch},
            {"sha256", V.Identity.SHA256},
            {"analysis_revision", V.Identity.Revision},
            {"architecture", V.Identity.Architecture},
            {"format", V.Identity.Format},
            {"abi", V.Identity.ABI},
            {"normalization", V.Identity.Normalization},
            {"analysis", V.Identity.Analysis},
            {"discovery_complete", V.Identity.DiscoveryComplete},
            {"functions", L.functions(I).size()}};
      });
  } else if (Q.Kind == "reports") {
    for (const auto &R : L.reports())
      B.append([&] {
        const auto &S = R.Evidence->Saved;
        return llvm::json::Object{
            {"id", R.Evidence->ID},
            {"primary_version", L.versions()[R.PrimaryVersion].ID},
            {"primary_label", L.versions()[R.PrimaryVersion].Label},
            {"secondary_version", L.versions()[R.SecondaryVersion].ID},
            {"secondary_label", L.versions()[R.SecondaryVersion].Label},
            {"state", stateName(S.State)},
            {"stopped_phase", S.StoppedPhase},
            {"incomplete", incomplete(S)},
            {"matches", S.Matches.size()},
            {"ambiguities", S.Ambiguities.size()},
            {"decisions", S.Overrides.size()},
            {"warnings", S.Warnings.size()},
            {"reviews", S.Reviews.Records.size()},
            {"native_region_hypotheses", S.Regions.Hypotheses.size()},
            {"flow_region_hypotheses", S.FlowRegions.Hypotheses.size()},
            {"bytes", R.Evidence->Text.size()}};
      });
  } else if (Q.Kind == "functions") {
    const auto V = L.version(Q.Version);
    require(V.has_value(), "unknown lineage snapshot");
    const auto Functions = L.functions(*V);
    for (uint32_t I = 0; I < Functions.size(); ++I) {
      const auto &F = Functions[I];
      B.charge(1 + F.Name.size() + Q.Search.size());
      if (!Q.Search.empty() &&
          !llvm::StringRef(F.Name).contains_insensitive(Q.Search) &&
          !llvm::StringRef(hex(F.Address)).contains_insensitive(Q.Search))
        continue;
      B.append([&] { return function(L, L.versions()[*V].FirstNode + I); });
    }
  } else if (Q.Kind == "edges") {
    for (uint32_t I = 0; I < L.edges().size(); ++I)
      B.append([&] { return edge(L, I); });
  } else if (Q.Kind == "conflicts") {
    for (uint32_t I = 0; I < L.conflicts().size(); ++I)
      B.append([&] { return conflict(L, I); });
  } else if (Q.Kind == "conflict_members") {
    require(Q.Index < L.conflicts().size(), "unknown lineage conflict");
    for (const auto N : L.conflicts()[Q.Index].Members)
      B.append([&] { return function(L, N); });
  } else {
    bool Found = Q.Report.empty();
    for (const auto &R : L.reports()) {
      B.charge();
      if (!Q.Report.empty() && Q.Report != R.Evidence->ID)
        continue;
      Found = true;
      const auto &S = R.Evidence->Saved;
      if (Q.Kind == "reviews") {
        std::set<std::string> Superseded;
        std::set<std::pair<uint64_t, uint64_t>> Active;
        for (const auto &M : S.Matches) {
          B.charge();
          Active.emplace(M.Primary, M.Secondary);
        }
        for (const auto &Review : S.Reviews.Records) {
          B.charge();
          if (!Review.Supersedes.empty())
            Superseded.insert(Review.Supersedes);
        }
        for (const auto &Review : S.Reviews.Records) {
          B.charge();
          const bool Latest = !Superseded.contains(Review.ID);
          if (Q.Latest && !Latest)
            continue;
          B.append([&] {
            auto Row = reviewRecordJSON(Review);
            Row["report"] = R.Evidence->ID;
            Row["latest"] = Latest;
            Row["active_pair"] =
                Active.contains({Review.Primary, Review.Secondary});
            Row["computed_patch_evidence"] = false;
            return Row;
          });
        }
      } else if (Q.Kind == "decisions") {
        for (uint32_t I = 0; I < S.Overrides.size(); ++I)
          B.append([&] {
            const auto &O = S.Overrides[I];
            return llvm::json::Object{
                {"index", I},
                {"primary", hex(O.Primary)},
                {"secondary", hex(O.Secondary)},
                {"kind", O.Kind == OverrideKind::Match     ? "match"
                         : O.Kind == OverrideKind::Unmatch ? "unmatch"
                                                           : "confirm"}};
          });
      } else if (Q.Kind == "native_regions") {
        for (uint32_t I = 0; I < S.Regions.Hypotheses.size(); ++I)
          B.append([&] {
            const auto &H = S.Regions.Hypotheses[I];
            llvm::json::Array Explanations;
            for (const auto K : H.Explanations)
              Explanations.push_back(regionKindName(K));
            return llvm::json::Object{
                {"index", I},
                {"id", H.Id},
                {"anchor_count", H.Anchors.size()},
                {"primary_function_count", H.PrimaryFunctions.size()},
                {"secondary_function_count", H.SecondaryFunctions.size()},
                {"explanations", std::move(Explanations)},
                {"primary_instructions", H.PrimaryInstructions},
                {"secondary_instructions", H.SecondaryInstructions},
                {"primary_coverage", H.PrimaryCoverage},
                {"secondary_coverage", H.SecondaryCoverage},
                {"confidence", H.Confidence},
                {"ambiguous", H.Ambiguous},
                {"analysis_complete", H.AnalysisComplete},
                {"native_authenticated", false},
                {"semantic_validation", "not_performed"}};
          });
      } else if (Q.Kind == "native_region_anchors") {
        require(Q.Index < S.Regions.Hypotheses.size(),
                "unknown native region hypothesis");
        for (const auto I : S.Regions.Hypotheses[Q.Index].Anchors)
          B.append([&] {
            require(I < S.Regions.Anchors.size(),
                    "saved native region anchor is outside its inventory");
            const auto &A = S.Regions.Anchors[I];
            // These are original snapshot coordinates, not freshly resolved
            // VAs.
            return llvm::json::Object{
                {"index", I},
                {"primary_function_index", A.Primary.Function},
                {"secondary_function_index", A.Secondary.Function},
                {"primary_block_index", A.Primary.Block},
                {"secondary_block_index", A.Secondary.Block},
                {"primary_instruction_index", A.Primary.FirstInstruction},
                {"secondary_instruction_index", A.Secondary.FirstInstruction},
                {"primary_instruction_count", A.Primary.InstructionCount},
                {"secondary_instruction_count", A.Secondary.InstructionCount},
                {"unique_seed", A.UniqueSeed}};
          });
      } else if (Q.Kind == "flow_regions") {
        for (uint32_t I = 0; I < S.FlowRegions.Hypotheses.size(); ++I)
          B.append([&] {
            auto Row = flowRegionHypothesisJSON(S.FlowRegions.Hypotheses[I]);
            Row["index"] = I;
            Row["native_authenticated"] = false;
            Row["profile"] = S.FlowRegions.Profile;
            return Row;
          });
      } else if (Q.Kind == "flow_region_anchors") {
        require(Q.Index < S.FlowRegions.Hypotheses.size(),
                "unknown SSA region hypothesis");
        for (const auto I : S.FlowRegions.Hypotheses[Q.Index].Anchors)
          B.append([&] {
            require(I < S.FlowRegions.Anchors.size(),
                    "saved SSA region anchor is outside its inventory");
            auto Row = flowRegionAnchorJSON(S.FlowRegions.Anchors[I]);
            Row["index"] = I;
            return Row;
          });
      } else if (Q.Kind == "warnings") {
        for (const auto &W : S.Warnings)
          B.append([&] { return llvm::json::Value(W); });
      } else if (Q.Kind == "ambiguities") {
        for (uint32_t I = 0; I < S.Ambiguities.size(); ++I)
          B.append([&] {
            const auto &A = S.Ambiguities[I];
            return llvm::json::Object{{"index", I},
                                      {"step", stepName(A.Step)},
                                      {"primary_count", A.Primary.size()},
                                      {"secondary_count", A.Secondary.size()}};
          });
      } else if (Q.Kind == "ambiguity_primary" ||
                 Q.Kind == "ambiguity_secondary") {
        require(Q.Index < S.Ambiguities.size(), "unknown source ambiguity");
        const auto &A = S.Ambiguities[Q.Index];
        const bool Secondary = Q.Kind == "ambiguity_secondary";
        for (const auto Address : Secondary ? A.Secondary : A.Primary)
          B.append([&] {
            const auto N = L.node(
                Secondary ? R.SecondaryVersion : R.PrimaryVersion, Address);
            require(N.has_value(), "source ambiguity has no admitted function");
            return function(L, *N);
          });
      } else
        throw std::invalid_argument("unknown lineage query kind");
    }
    require(Found, "unknown lineage source report");
  }
  require(Q.Offset <= B.Total, "lineage offset exceeds the collection");
  const auto Next = Q.Offset + B.Rows.size();
  Result.JSON = llvm::json::Object{
      {"schema", "neverd.bindiff.lineage.page"},
      {"schema_version", LineageSchemaVersion},
      {"revision", L.revision()},
      {"kind", Q.Kind},
      {"provenance", "retained_reports"},
      {"total", B.Total},
      {"offset", Q.Offset},
      {"next_offset",
       Next < B.Total ? llvm::json::Value(Next) : llvm::json::Value(nullptr)},
      {"work", B.Work},
      {"items", std::move(B.Rows)}};
  Control.report("lineage:query_publish");
  return Result;
}
} // namespace neverd::diff
