//===- Lineage.cpp - Retained multi-version correspondence evidence -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Builds an immutable graph from the shared saved reader. Conflicts retain
/// whole candidate groups and one witness tree instead of enumerating paths.
//===----------------------------------------------------------------------===//

#include "neverd/diff/Lineage.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <utility>

namespace neverd::diff {
namespace {
std::string defaultLabel(const ProgramIdentity &Identity) {
  const llvm::StringRef Path(Identity.Path);
  if (Path.empty())
    return Identity.SHA256;
  const auto Separator = Path.find_last_of("/\\");
  const auto Name = Separator == llvm::StringRef::npos
                        ? Path
                        : Path.drop_front(Separator + 1);
  return Name.empty() ? Path.str() : Name.str();
}

void require(bool Condition, llvm::StringRef Error) {
  if (!Condition)
    throw std::invalid_argument(Error.str());
}

struct Budget {
  uint64_t &Work;
  uint64_t Maximum;
  const ComparisonControl &Control;
  void charge(uint64_t Amount = 1) {
    Control.check();
    if (Amount > Maximum - Work)
      throw ComparisonInterrupted(ComparisonState::ResourceLimited);
    Work += Amount;
  }
};

std::string digest(llvm::StringRef Text, Budget &B) {
  llvm::SHA256 Hash;
  while (!Text.empty()) {
    const auto Chunk =
        Text.take_front(limits::diff::kSavedComparisonChunkBytes);
    B.charge(Chunk.size());
    Hash.update(Chunk);
    Text = Text.drop_front(Chunk.size());
  }
  B.Control.check();
  return llvm::toHex(Hash.final(), true);
}

void fields(const llvm::json::Object &O,
            std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &Field : O)
    require(llvm::is_contained(Allowed, llvm::StringRef(Field.first)),
            "unknown lineage field: " + Field.first.str());
}

std::string text(const llvm::json::Object &O, llvm::StringRef Key) {
  const auto Value = O.getString(Key);
  require(Value.has_value(), "lineage field must be a string: " + Key.str());
  return Value->str();
}

bool sameInventory(llvm::ArrayRef<SavedFunction> A,
                   llvm::ArrayRef<SavedFunction> B, Budget &Work) {
  if (A.size() != B.size())
    return false;
  for (size_t I = 0; I < A.size(); ++I) {
    Work.charge(1 + A[I].Name.size() + B[I].Name.size());
    if (A[I].Address != B[I].Address || A[I].Size != B[I].Size ||
        A[I].Name != B[I].Name || A[I].Traits != B[I].Traits ||
        A[I].Blocks != B[I].Blocks || A[I].Instructions != B[I].Instructions)
      return false;
  }
  return true;
}

void allowance(uint64_t Used, uint64_t Additional, uint64_t Maximum) {
  if (Used > Maximum || Additional > Maximum - Used)
    throw ComparisonInterrupted(ComparisonState::ResourceLimited);
}

} // namespace

std::optional<std::string> LineageSettings::validate() const {
  if (!MaxSourceBytes ||
      MaxSourceBytes > limits::diff::kMaxSavedComparisonBytes)
    return "lineage source byte allowance is outside saved-document bounds";
  if (!MaxNodes || MaxNodes == NoIndex || !MaxEdges || MaxEdges == NoIndex)
    return "lineage node and edge allowances must be positive addressable "
           "counts";
  if (!MaxWork || MaxWork > uint64_t(INT64_MAX))
    return "lineage work allowance must be a positive signed 64-bit count";
  return std::nullopt;
}

llvm::ArrayRef<SavedFunction> Lineage::functions(uint32_t Index) const {
  if (Index >= Versions.size())
    throw std::invalid_argument("unknown lineage version");
  const auto &V = Versions[Index];
  const auto &Saved = Reports[V.Report].Evidence->Saved;
  return V.Secondary ? Saved.SecondaryFunctions : Saved.PrimaryFunctions;
}

std::optional<uint32_t> Lineage::version(llvm::StringRef ID) const {
  const auto It =
      llvm::lower_bound(Versions, ID, [](const auto &V, llvm::StringRef Key) {
        return V.ID < Key;
      });
  if (It == Versions.end() || It->ID != ID)
    return std::nullopt;
  return uint32_t(It - Versions.begin());
}

std::optional<uint32_t> Lineage::node(uint32_t Version,
                                      uint64_t Address) const {
  if (Version >= Versions.size())
    return std::nullopt;
  const auto Functions = functions(Version);
  const auto It =
      llvm::lower_bound(Functions, Address, [](const auto &F, uint64_t A) {
        return F.Address < A;
      });
  if (It == Functions.end() || It->Address != Address)
    return std::nullopt;
  return Versions[Version].FirstNode + uint32_t(It - Functions.begin());
}

std::shared_ptr<const Lineage> readLineage(llvm::StringRef Text,
                                           const LineageSettings &Settings,
                                           std::string &Error,
                                           const ComparisonControl &Control,
                                           const Lineage *Cache) {
  Error.clear();
  Control.check();
  try {
    if (const auto Invalid = Settings.validate())
      throw std::invalid_argument(*Invalid);
    require(Text.size() <= limits::diff::kMaxSavedComparisonBytes,
            "lineage document exceeds saved-document byte allowance");
    auto Out = std::shared_ptr<Lineage>(new Lineage);
    Budget B{Out->Work, Settings.MaxWork, Control};
    B.charge(Text.size());
    Control.report("lineage:parse", 0, Text.size());
    auto Document = llvm::json::parse(Text);
    if (!Document) {
      const auto ParseError = llvm::toString(Document.takeError());
      Control.report("lineage:parse", Text.size(), Text.size());
      throw std::invalid_argument(ParseError);
    }
    Control.report("lineage:parse", Text.size(), Text.size());
    const auto *Root = Document->getAsObject();
    require(Root, "lineage document must be an object");
    fields(*Root, {"schema", "schema_version", "reports", "versions"});
    require(Root->getString("schema") == LineageSchema &&
                Root->getInteger("schema_version") == LineageSchemaVersion,
            "unsupported lineage schema or version");
    const auto *Reports = Root->getArray("reports");
    require(Reports && !Reports->empty(), "lineage requires saved reports");
    std::map<std::string, std::shared_ptr<const LineageEvidence>> Sources,
        Prior;
    uint64_t HeldBytes = Cache ? Cache->sourceBytes() : 0;
    allowance(HeldBytes, 0, Settings.MaxSourceBytes);
    if (Cache)
      for (const auto &R : Cache->reports()) {
        B.charge();
        Prior.emplace(R.Evidence->ID, R.Evidence);
      }
    for (size_t I = 0; I < Reports->size(); ++I) {
      Control.report("lineage:reports", I, Reports->size());
      const auto Raw = (*Reports)[I].getAsString();
      require(Raw.has_value(),
              "lineage report must contain its original JSON string");
      const auto ID = digest(*Raw, B);
      if (Sources.contains(ID))
        continue;
      allowance(Out->ReportBytes, Raw->size(), Settings.MaxSourceBytes);
      Out->ReportBytes += Raw->size();
      if (const auto It = Prior.find(ID); It != Prior.end()) {
        B.charge(Raw->size());
        require(It->second->Text == *Raw, "lineage report digest collision");
        Sources.emplace(ID, It->second);
        continue;
      }
      allowance(HeldBytes, Raw->size(), Settings.MaxSourceBytes);
      HeldBytes += Raw->size();
      B.charge(Raw->size());
      std::string ReadError;
      auto Saved = readComparison(*Raw, ReadError, Control);
      if (!Saved)
        throw std::invalid_argument("invalid lineage report " + ID + ": " +
                                    ReadError);
      auto Evidence = std::make_shared<LineageEvidence>();
      Evidence->ID = ID;
      Evidence->Text = Raw->str();
      Evidence->Saved = std::move(*Saved);
      Sources.emplace(ID, std::move(Evidence));
    }
    Control.report("lineage:reports", Reports->size(), Reports->size());
    require(Sources.size() < NoIndex, "too many lineage reports");
    std::map<std::string, LineageVersion> Versions;
    for (const auto &[ID, Evidence] : Sources) {
      B.charge();
      const auto Report = uint32_t(Out->Reports.size());
      Out->Reports.push_back({Evidence});
      for (bool Secondary : {false, true}) {
        const auto &S = Evidence->Saved;
        const auto &Identity = Secondary ? S.Secondary : S.Primary;
        const auto &Inventory =
            Secondary ? S.SecondaryFunctions : S.PrimaryFunctions;
        require(!Identity.SHA256.empty() && !Identity.Revision.empty(),
                "lineage needs known input hashes and analysis revisions");
        const auto Key = programAnalysisIdentity(Identity);
        const auto VersionID = digest(Key, B);
        auto [It, Added] = Versions.try_emplace(VersionID);
        if (Added) {
          auto &V = It->second;
          V.ID = VersionID;
          V.Identity = Identity;
          V.Label = defaultLabel(Identity);
          V.Report = Report;
          V.Secondary = Secondary;
        } else {
          auto &V = It->second;
          const auto &Previous = Out->Reports[V.Report].Evidence->Saved;
          const auto &OldInventory = V.Secondary ? Previous.SecondaryFunctions
                                                 : Previous.PrimaryFunctions;
          require(programAnalysisIdentity(V.Identity) == Key &&
                      sameInventory(OldInventory, Inventory, B),
                  "reports disagree about one lineage snapshot inventory");
          if (!Identity.Path.empty() &&
              (V.Identity.Path.empty() || defaultLabel(Identity) < V.Label)) {
            V.Label = defaultLabel(Identity);
            V.Identity.Path = Identity.Path;
          }
        }
      }
    }
    require(Versions.size() < NoIndex, "too many lineage versions");
    if (const auto *Metadata = Root->get("versions")) {
      const auto *Rows = Metadata->getAsArray();
      require(Rows, "lineage versions metadata must be an array");
      std::set<std::string> Seen;
      for (const auto &Row : *Rows) {
        B.charge();
        const auto *O = Row.getAsObject();
        require(O, "lineage version metadata must be an object");
        fields(*O, {"id", "label", "branch"});
        const auto ID = text(*O, "id");
        auto It = Versions.find(ID);
        require(It != Versions.end() && Seen.insert(ID).second,
                "lineage metadata names an unknown or repeated version");
        It->second.Label = text(*O, "label");
        It->second.Branch = text(*O, "branch");
        require(!llvm::StringRef(It->second.Label).trim().empty(),
                "lineage version label must not be empty");
      }
    }
    const uint64_t PriorNodes = Cache ? Cache->nodes().size() : 0;
    uint64_t PriorEdges = Cache ? Cache->edges().size() : 0;
    if (Cache)
      for (const auto &Conflict : Cache->conflicts()) {
        B.charge();
        PriorEdges += Conflict.Kind == LineageConflictKind::RejectedPair;
      }
    for (auto &[ID, Version] : Versions) {
      B.charge();
      const auto Metadata = Version.Label.size() + Version.Branch.size();
      allowance(Out->sourceBytes(), Metadata, Settings.MaxSourceBytes);
      allowance(HeldBytes, Metadata, Settings.MaxSourceBytes);
      Out->MetadataBytes += Metadata;
      HeldBytes += Metadata;
      Version.FirstNode = uint32_t(Out->Nodes.size());
      const auto Index = uint32_t(Out->Versions.size());
      Out->Versions.push_back(std::move(Version));
      const auto Inventory = Out->functions(Index);
      allowance(PriorNodes + Out->Nodes.size(), Inventory.size(),
                Settings.MaxNodes);
      B.charge(Inventory.size());
      for (uint32_t F = 0; F < Inventory.size(); ++F) {
        Control.check();
        Out->Nodes.push_back({Index, F, uint32_t(Out->Nodes.size())});
      }
    }
    B.charge(Out->Nodes.size());
    Out->Adjacency.resize(Out->Nodes.size());
    std::vector<uint32_t> Parent(Out->Nodes.size()), Size(Out->Nodes.size(), 1);
    std::iota(Parent.begin(), Parent.end(), uint32_t{0});
    const auto Find = [&](uint32_t V) {
      while (Parent[V] != V) {
        B.charge();
        Parent[V] = Parent[Parent[V]];
        V = Parent[V];
      }
      return V;
    };
    struct Rejection {
      uint32_t Primary, Secondary, Report;
    };
    std::vector<Rejection> Rejections;
    for (uint32_t R = 0; R < Out->Reports.size(); ++R) {
      Control.report("lineage:graph", R, Out->Reports.size());
      auto &Report = Out->Reports[R];
      const auto &Saved = Report.Evidence->Saved;
      Report.PrimaryVersion =
          *Out->version(digest(programAnalysisIdentity(Saved.Primary), B));
      Report.SecondaryVersion =
          *Out->version(digest(programAnalysisIdentity(Saved.Secondary), B));
      allowance(PriorEdges + Out->Edges.size() + Rejections.size(),
                Saved.Matches.size(), Settings.MaxEdges);
      for (uint32_t M = 0; M < Saved.Matches.size(); ++M) {
        B.charge();
        const auto &Match = Saved.Matches[M];
        const auto P = *Out->node(Report.PrimaryVersion, Match.Primary);
        const auto S = *Out->node(Report.SecondaryVersion, Match.Secondary);
        const auto Edge = uint32_t(Out->Edges.size());
        Out->Edges.push_back({P, S, R, M});
        Out->Adjacency[P].push_back(Edge);
        if (S != P)
          Out->Adjacency[S].push_back(Edge);
        auto A = Find(P), C = Find(S);
        if (A != C) {
          if (Size[A] < Size[C])
            std::swap(A, C);
          Parent[C] = A;
          Size[A] += Size[C];
        }
      }
      B.charge(Saved.Overrides.size());
      for (const auto &[P, S] : rejectedPairs(Saved.Overrides, Control)) {
        B.charge();
        const auto A = Out->node(Report.PrimaryVersion, P);
        const auto C = Out->node(Report.SecondaryVersion, S);
        if (A && C) {
          allowance(PriorEdges + Out->Edges.size() + Rejections.size(), 1,
                    Settings.MaxEdges);
          Rejections.push_back({*A, *C, R});
        }
      }
    }
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> Groups;
    for (uint32_t N = 0; N < Out->Nodes.size(); ++N) {
      B.charge();
      auto &Node = Out->Nodes[N];
      Node.Component = Find(N);
      Groups[{Node.Component, Node.Version}].push_back(N);
    }
    for (auto &[Key, Members] : Groups) {
      B.charge(Members.size());
      if (Members.size() > 1)
        Out->Conflicts.push_back({LineageConflictKind::MultipleFunctions,
                                  Key.first, Key.second, NoIndex,
                                  std::move(Members)});
    }
    for (const auto &R : Rejections) {
      B.charge();
      if (Out->Nodes[R.Primary].Component == Out->Nodes[R.Secondary].Component)
        Out->Conflicts.push_back({LineageConflictKind::RejectedPair,
                                  Out->Nodes[R.Primary].Component,
                                  NoIndex,
                                  R.Report,
                                  {R.Primary, R.Secondary}});
    }
    B.charge(Out->Nodes.size());
    Out->ComponentConflicts.resize(Out->Nodes.size());
    require(Out->Conflicts.size() < NoIndex,
            "too many lineage conflict groups");
    for (uint32_t I = 0; I < Out->Conflicts.size(); ++I) {
      B.charge();
      Out->ComponentConflicts[Out->Conflicts[I].Component].push_back(I);
    }
    // The revision binds source report digests and metadata, not caller order.
    llvm::json::Array SourceIDs, Labels;
    for (const auto &R : Out->Reports) {
      B.charge();
      SourceIDs.push_back(R.Evidence->ID);
    }
    for (const auto &V : Out->Versions) {
      B.charge();
      Labels.push_back(llvm::json::Array{V.ID, V.Label, V.Branch});
    }
    std::string RevisionText;
    llvm::raw_string_ostream OS(RevisionText);
    OS << llvm::json::Value(llvm::json::Array{
        "neverd.bindiff.lineage-v1", std::move(SourceIDs), std::move(Labels)});
    Out->Revision = digest(RevisionText, B);
    Control.report("lineage:publish");
    return Out;
  } catch (const ComparisonInterrupted &) {
    throw;
  } catch (const std::invalid_argument &E) {
    Error = E.what();
    return nullptr;
  }
}

LineageTrace Lineage::trace(uint32_t Source, uint64_t MaxWork,
                            const ComparisonControl &Control) const {
  Control.check();
  require(Source < Nodes.size(), "unknown lineage source function");
  LineageTrace Out;
  Out.Revision = Revision;
  Out.Source = Source;
  Budget B{Out.Work, MaxWork, Control};
  B.charge(Nodes.size() + Edges.size());
  std::vector<uint32_t> Seen(Nodes.size(), NoIndex);
  std::vector<bool> EdgeSeen(Edges.size());
  Seen[Source] = 0;
  Out.Nodes.push_back({Source, NoIndex, NoIndex, 0});
  for (size_t Next = 0; Next < Out.Nodes.size(); ++Next) {
    Control.report("lineage:trace", Next, Nodes.size());
    const auto Current = Out.Nodes[Next];
    B.charge();
    for (const auto E : Adjacency[Current.Node]) {
      B.charge();
      if (!EdgeSeen[E]) {
        EdgeSeen[E] = true;
        Out.Edges.push_back(E);
      }
      const auto &Edge = Edges[E];
      const auto Other =
          Edge.Primary == Current.Node ? Edge.Secondary : Edge.Primary;
      if (Seen[Other] != NoIndex)
        continue;
      Seen[Other] = uint32_t(Out.Nodes.size());
      Out.Nodes.push_back({Other, Current.Node, E, Current.Distance + 1});
    }
  }
  for (const auto C : ComponentConflicts[Nodes[Source].Component]) {
    B.charge(1 + Conflicts[C].Members.size());
    Out.Conflicts.push_back(C);
  }
  Control.report("lineage:trace", Out.Nodes.size(), Out.Nodes.size());
  return Out;
}

std::string writeLineage(const Lineage &Value,
                         const ComparisonControl &Control) {
  llvm::json::Array Reports, Versions;
  for (const auto &R : Value.reports()) {
    Control.check();
    Reports.push_back(R.Evidence->Text);
  }
  for (const auto &V : Value.versions()) {
    Control.check();
    Versions.push_back(llvm::json::Object{
        {"id", V.ID}, {"label", V.Label}, {"branch", V.Branch}});
  }
  Control.check();
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << llvm::json::Value(
      llvm::json::Object{{"schema", LineageSchema},
                         {"schema_version", LineageSchemaVersion},
                         {"reports", std::move(Reports)},
                         {"versions", std::move(Versions)}});
  Control.check();
  allowance(0, Text.size(), limits::diff::kMaxSavedComparisonBytes);
  return Text;
}

} // namespace neverd::diff
