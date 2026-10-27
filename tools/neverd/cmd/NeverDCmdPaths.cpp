//===- NeverDCmdPaths.cpp - paths and calltree subcommands ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd paths`: the call paths between functions, segment by segment
/// through waypoints, as an interactive disassembler's path finder shows them.
/// `neverd calltree`: the functions a function calls, or that call it, to a
/// depth.  Both read the engine's graph of direct references
/// (neverd_call_paths_json, neverd_call_tree_json) without analysis.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace neverd::cli {
namespace {

/// A function or import as the engine's graph queries take it: a function's
/// name as given, else an address as hexadecimal, else the text itself (an
/// import's name).
std::string nodeArgument(neverd_session_t Sess, StringRef Text) {
  if (Text.starts_with_insensitive("0x") ||
      neverd_func_find_by_name(Sess, Text.str().c_str()) >= 0)
    return Text.str();
  if (const auto Address = parseAddrArg(Text))
    return "0x" + utohexstr(*Address);
  return Text.str();
}

json::Array nodeArguments(neverd_session_t Sess,
                          const std::vector<std::string> &Texts) {
  json::Array Out;
  for (const std::string &Text : Texts)
    Out.push_back(nodeArgument(Sess, Text));
  return Out;
}

/// Runs \p Query with \p Options and prints its reply as JSON, or hands the
/// parsed reply to \p Print.
template <typename PrintT>
int runGraphQuery(neverd_session_t Sess,
                  const char *(*Query)(neverd_session_t, const char *),
                  const json::Object &Options, PrintT Print) {
  std::string Text;
  raw_string_ostream(Text) << json::Value(json::Object(Options));
  const char *Reply = Query(Sess, Text.c_str());
  if (!Reply) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  const std::string Owned(Reply);
  neverd_free_string(Reply);
  if (JsonOutput) {
    outs() << Owned << "\n";
    return 0;
  }
  auto Parsed = json::parse(Owned);
  const json::Object *Root = Parsed ? Parsed->getAsObject() : nullptr;
  if (!Root) {
    if (!Parsed)
      consumeError(Parsed.takeError());
    WithColor::error() << "malformed reply from the engine\n";
    return 1;
  }
  return Print(*Root);
}

/// "name (0x...)" or "name (import, 0x...)" for each node a reply describes.
std::map<std::string, std::string> nodeLabels(const json::Object &Root) {
  std::map<std::string, std::string> Labels;
  if (const json::Array *Nodes = Root.getArray("nodes"))
    for (const json::Value &Node : *Nodes)
      if (const json::Object *Object = Node.getAsObject()) {
        const std::string Address =
            Object->getString("address").value_or("").str();
        std::string Label = Object->getString("name").value_or("").str();
        std::string Notes;
        if (Object->getString("kind").value_or("") == "import")
          Notes = "import, ";
        if (Object->getBoolean("library").value_or(false))
          Notes += "library, ";
        Labels[Address] = Label + " (" + Notes + Address + ")";
      }
  return Labels;
}

std::string shortLabel(const std::map<std::string, std::string> &Labels,
                       StringRef Address) {
  const auto It = Labels.find(Address.str());
  if (It == Labels.end())
    return Address.str();
  // The name alone: paths repeat nodes the summary already names in full.
  return StringRef(It->second).split(" (").first.str();
}

std::string label(const std::map<std::string, std::string> &Labels,
                  StringRef Address) {
  const auto It = Labels.find(Address.str());
  return It == Labels.end() ? Address.str() : It->second;
}

int printPaths(const json::Object &Root) {
  const auto Labels = nodeLabels(Root);
  const int64_t MaxDepth = Root.getInteger("max_depth").value_or(0);
  const json::Array *Segments = Root.getArray("segments");
  if (!Segments)
    return 0;
  for (const json::Value &Value : *Segments) {
    const json::Object *Segment = Value.getAsObject();
    if (!Segment)
      continue;
    const StringRef From = Segment->getString("from").value_or("");
    const StringRef To = Segment->getString("to").value_or("");
    outs() << label(Labels, From) << " -> " << label(Labels, To) << "\n";
    const auto Shortest = Segment->getInteger("shortest");
    if (!Shortest) {
      outs() << "  no path of at most " << MaxDepth << " edges either way\n";
      continue;
    }
    if (Segment->getBoolean("reversed").value_or(false))
      outs() << "  (only the reverse direction connects)\n";
    outs() << "  shortest " << *Shortest
           << (*Shortest == 1 ? " edge" : " edges") << "; "
           << Segment->getInteger("node_count").value_or(0) << " nodes and "
           << Segment->getInteger("edge_count").value_or(0)
           << " edges on paths of at most " << MaxDepth << "\n";
    unsigned Number = 0;
    if (const json::Array *Paths = Segment->getArray("paths"))
      for (const json::Value &Path : *Paths) {
        const json::Array *Steps = Path.getAsArray();
        if (!Steps)
          continue;
        outs() << "  " << ++Number << ".";
        bool First = true;
        for (const json::Value &Step : *Steps) {
          outs() << (First ? " " : " -> ")
                 << shortLabel(Labels, Step.getAsString().value_or(""));
          First = false;
        }
        outs() << "\n";
      }
    if (!Segment->getBoolean("complete").value_or(true))
      outs() << "  (not every path is listed; raise --max-paths or lower "
                "--max-depth)\n";
  }
  return 0;
}

int printTree(const json::Object &Root) {
  const auto Labels = nodeLabels(Root);
  const bool Callees = Root.getString("direction").value_or("") == "callees";
  // Each node under the first node one level nearer the root that it is
  // listed with, as a breadth-first search found it.
  std::map<std::string, int64_t> Depth;
  std::vector<std::string> Order;
  if (const json::Array *Nodes = Root.getArray("nodes"))
    for (const json::Value &Node : *Nodes)
      if (const json::Object *Object = Node.getAsObject()) {
        const std::string Address =
            Object->getString("address").value_or("").str();
        Depth[Address] = Object->getInteger("depth").value_or(0);
        Order.push_back(Address);
      }
  std::map<std::string, std::vector<std::string>> Children;
  std::map<std::string, std::string> Parent;
  if (const json::Array *Edges = Root.getArray("edges"))
    for (const json::Value &Edge : *Edges) {
      const json::Array *Row = Edge.getAsArray();
      if (!Row || Row->size() < 2)
        continue;
      std::string Near = (*Row)[0].getAsString().value_or("").str();
      std::string Far = (*Row)[1].getAsString().value_or("").str();
      if (!Callees)
        std::swap(Near, Far);
      if (!Depth.count(Near) || !Depth.count(Far) ||
          Depth[Far] != Depth[Near] + 1 || Parent.count(Far))
        continue;
      Parent[Far] = Near;
      Children[Near].push_back(Far);
    }
  const auto Print = [&](auto &&Self, const std::string &Node,
                         unsigned Indent) -> void {
    outs().indent(Indent) << label(Labels, Node) << "\n";
    for (const std::string &Child : Children[Node])
      Self(Self, Child, Indent + 2);
  };
  if (!Order.empty())
    Print(Print, Order.front(), 0);
  if (Root.getBoolean("depth_limited").value_or(false))
    outs() << "(deeper " << (Callees ? "callees" : "callers")
           << " exist; raise --max-depth)\n";
  if (!Root.getBoolean("complete").value_or(true))
    outs() << "(the list was cut at its limit)\n";
  return 0;
}

} // namespace

int runPaths(neverd_session_t Sess) {
  if (PathsFrom.empty() || PathsTo.empty()) {
    WithColor::error() << "paths requires --from and --to\n";
    return 1;
  }
  json::Array Waypoints{nodeArgument(Sess, PathsFrom)};
  for (json::Value &Via : nodeArguments(Sess, PathsVia))
    Waypoints.push_back(std::move(Via));
  Waypoints.push_back(nodeArgument(Sess, PathsTo));
  json::Object Options{{"waypoints", std::move(Waypoints)},
                       {"exclude", nodeArguments(Sess, GraphExclude)},
                       {"shortest_only", bool(PathsShortest)},
                       {"data_refs", bool(GraphDataRefs)}};
  if (GraphMaxDepth.getNumOccurrences())
    Options["max_depth"] = int64_t(GraphMaxDepth);
  if (PathsMaxPaths.getNumOccurrences())
    Options["max_paths"] = int64_t(PathsMaxPaths);
  return runGraphQuery(Sess, neverd_call_paths_json, Options, printPaths);
}

int runCallTree(neverd_session_t Sess) {
  if (CallTreeFunc.empty()) {
    WithColor::error() << "calltree requires --func\n";
    return 1;
  }
  json::Object Options{{"root", nodeArgument(Sess, CallTreeFunc)},
                       {"direction", CallTreeCallers ? "callers" : "callees"},
                       {"exclude", nodeArguments(Sess, GraphExclude)},
                       {"data_refs", bool(GraphDataRefs)},
                       {"expand_library", bool(CallTreeExpandLibrary)}};
  if (GraphMaxDepth.getNumOccurrences())
    Options["max_depth"] = int64_t(GraphMaxDepth);
  return runGraphQuery(Sess, neverd_call_tree_json, Options, printTree);
}

} // namespace neverd::cli
