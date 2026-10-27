//===- Tools.cpp - The MCP server's tools and what they return ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The tool catalog, argument checks, and the model-facing result shapes.
///
//===----------------------------------------------------------------------===//

#include "Tools.h"

#include "Reading.h"
#include "ResourceLimits.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>

namespace neverd::binmcp {
namespace {
using Ordered = nlohmann::ordered_json;

//===----------------------------------------------------------------------===//
// The catalog
//===----------------------------------------------------------------------===//

Json integer(std::int64_t Minimum, std::int64_t Maximum) {
  return {{"type", "integer"}, {"minimum", Minimum}, {"maximum", Maximum}};
}

Tool tool(const char *Name, const char *Operation, const char *Description,
          Json Properties = Json::object(),
          std::vector<std::string> Required = {}) {
  // A revision guards the continuation of a page against analysis that
  // publishes a newer one meanwhile.
  if (Properties.contains("offset"))
    Properties["expected_revision"] = {{"type", "string"}};
  Json Schema{{"type", "object"},
              {"properties", std::move(Properties)},
              {"additionalProperties", false}};
  if (!Required.empty())
    Schema["required"] = std::move(Required);
  Json Listed{{"name", Name},
              {"description", Description},
              {"inputSchema", std::move(Schema)},
              {"annotations",
               {{"readOnlyHint", true},
                {"destructiveHint", false},
                {"idempotentHint", true},
                {"openWorldHint", false}}}};
  return {Operation, std::move(Listed)};
}

// An exact hexadecimal address, or a name a function, import or label goes
// by: the "address" format, which validate() checks.
Json address() {
  return {{"type", "string"},
          {"maxLength", ::neverd::limits::mcp::kToolsAddressMaxLength},
          {"format", "address"}};
}

/// A bounded page, with what else the tool takes.
Json page(Json Properties = Json::object()) {
  Json Page{
      {"offset", integer(0, 2147483647)},
      {"limit",
       integer(1, ::neverd::limits::mcp::kToolsPagePageLimitIntegerMaximum)}};
  Page.update(Properties);
  return Page;
}

const Json Filter{{"type", "string"},
                  {"maxLength", ::neverd::limits::mcp::kToolsFilterMaxLength}};
// A function or import by name, or by exact hexadecimal address.
const Json Node{{"type", "string"},
                {"minLength", 1},
                {"maxLength", ::neverd::limits::mcp::kToolsNodeMaxLength}};
const Json Flag{{"type", "boolean"}};

Json nodes() {
  return {{"type", "array"},
          {"items", Node},
          {"maxItems", ::neverd::limits::mcp::kToolsNodesMaxItems}};
}

std::vector<Tool> catalog() {
  const auto Depth =
      integer(1, ::neverd::limits::mcp::kToolsCatalogDepthIntegerMaximum);
  return {
      tool("project_metadata", "metadata",
           "Read metadata for the explicitly opened NeverD input."),
      tool("list_functions", "functions",
           "Read a bounded function page; preserve revision across pages.",
           page({{"filter", Filter}})),
      tool("read_disassembly", "disasm",
           "Read an instruction window at an address (0x... or a name).",
           {{"address", address()},
            {"limit",
             integer(1, ::neverd::limits::mcp::
                            kToolsCatalogReadDisassemblyLimitIntegerMaximum)}},
           {"address"}),
      tool(
          "read_decompilation", "decompile",
          "Read one function's pseudocode (\"source\": its own language; or "
          "C, Rust, Go) or IR. Starts at the definition; prelude=true adds "
          "the includes and declarations that make it compile. C is "
          "abbreviated as the header's legend says; exact=true gives the "
          "engine's spelling.",
          page({{"limit",
                 integer(
                     1, ::neverd::limits::mcp::
                            kToolsCatalogReadDecompilationLimitIntegerMaximum)},
                {"address", address()},
                {"prelude", Flag},
                {"exact", Flag},
                {"representation",
                 {{"type", "string"},
                  {"enum",
                   {"source", "c", "rust", "go", "llvmc", "low", "med", "high",
                    "llvm"}}}}}),
          {"address"}),
      tool("read_bytes", "bytes",
           "Read up to 4096 mapped bytes at a virtual address.",
           {{"address", address()},
            {"size", integer(1, ::neverd::limits::mcp::
                                    kToolsCatalogReadBytesSizeIntegerMaximum)}},
           {"address"}),
      tool(
          "list_xrefs", "xrefs",
          "Read references associated with a virtual address.",
          page({{"address", address()},
                {"direction", {{"type", "string"}, {"enum", {"to", "from"}}}}}),
          {"address"}),
      tool("list_strings", "strings",
           "Read a bounded string page; filter matches text or address. Long "
           "texts are cut to text_limit bytes (default 200) with an "
           "ellipsis.",
           page({{"filter", Filter},
                 {"text_limit",
                  integer(
                      1,
                      ::neverd::limits::mcp::
                          kToolsCatalogListStringsTextLimitIntegerMaximum)}})),
      tool("read_cfg", "cfg", "Read the control-flow graph for one function.",
           {{"address", address()}}, {"address"}),
      tool("list_imports", "imports",
           "Read imported symbols by module; filter matches name or module.",
           page({{"filter", Filter}})),
      tool("find_string_references", "string_references",
           "Find the instructions, and their functions, that use strings "
           "matching filter (text, function or address).",
           page({{"filter", Filter}})),
      tool("find_call_paths", "call_paths",
           "Call paths from one function or import to another (name or 0x "
           "address), through optional via waypoints in order; shortest "
           "first. data_refs also follows taken addresses.",
           {{"from", Node},
            {"to", Node},
            {"via", nodes()},
            {"exclude", nodes()},
            {"max_depth", Depth},
            {"shortest_only", Flag},
            {"data_refs", Flag},
            {"max_paths",
             integer(1, ::neverd::limits::mcp::
                            kToolsCatalogFindCallPathsMaxPathsIntegerMaximum)}},
           {"from", "to"}),
      tool("list_call_tree", "call_tree",
           "What a function (name or 0x address) calls, or with callers what "
           "calls it, to max_depth levels (default 1).",
           {{"function", Node},
            {"callers", Flag},
            {"max_depth", Depth},
            {"data_refs", Flag},
            {"exclude", nodes()}},
           {"function"}),
      tool(
          "search", "search",
          "Find hex bytes (\"48 89 e5\"), text or an immediate operand "
          "(\"0x40000000\"), in address order; from=next_address "
          "continues.",
          {{"kind",
            {{"type", "string"}, {"enum", {"bytes", "text", "immediate"}}}},
           {"pattern", {{"type", "string"}, {"minLength", 1}}},
           {"from", {{"type", "string"}}},
           {"limit",
            integer(
                1,
                ::neverd::limits::mcp::kToolsCatalogSearchLimitIntegerMaximum)},
           {"case_sensitive", Flag}},
          {"kind", "pattern"}),
  };
}

std::vector<Tool> workbenchCatalog() {
  std::vector<Tool> Tools{
      tool("current_selection", "selection",
           "Read the current selection in the attached GUI session."),
      tool("navigate_gui", "navigate",
           "Navigate the attached GUI to an explicit virtual address.",
           {{"address", address()}}, {"address"}),
      tool("highlight_gui", "highlight",
           "Highlight an explicit address in the attached GUI.",
           {{"address", address()}}, {"address"}),
  };
  // Navigation changes what the workbench shows.
  for (auto &Navigation : Tools)
    if (Navigation.Operation != "selection")
      Navigation.Listed["annotations"]["readOnlyHint"] = false;
  return Tools;
}

//===----------------------------------------------------------------------===//
// Argument checks
//===----------------------------------------------------------------------===//

/// The characters, that is Unicode code points, a JSON Schema length counts.
std::size_t characters(std::string_view Text) {
  return static_cast<std::size_t>(
      std::count_if(Text.begin(), Text.end(), [](char Byte) {
        return (static_cast<unsigned char>(Byte) & 0xC0) != 0x80;
      }));
}

bool within(const Json &Value, std::int64_t Minimum, std::int64_t Maximum) {
  if (Value.is_number_unsigned()) {
    const auto Number = Value.get<std::uint64_t>();
    return Maximum >= 0 && Number <= static_cast<std::uint64_t>(Maximum) &&
           (Minimum <= 0 || Number >= static_cast<std::uint64_t>(Minimum));
  }
  const auto Number = Value.get<std::int64_t>();
  return Minimum <= Number && Number <= Maximum;
}

/// Whether \p Text names an address: exactly 0x and 1-16 hexadecimal
/// digits, or a name, which neither starts with 0x nor holds a line break.
bool namesAddress(std::string_view Text) {
  if (hexAddress(Text))
    return true;
  const bool Hex =
      Text.size() >= 2 && Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X');
  return !Text.empty() && !Hex &&
         Text.find_first_of("\r\n") == std::string_view::npos &&
         Text.find("\xE2\x80\xA8") == std::string_view::npos &&
         Text.find("\xE2\x80\xA9") == std::string_view::npos;
}

bool fits(const Json &Value, const Json &Rule) {
  const auto &Type = Rule.at("type").get_ref<const std::string &>();
  if (Type == "integer")
    return Value.is_number_integer() &&
           within(Value, Rule.at("minimum").get<std::int64_t>(),
                  Rule.at("maximum").get<std::int64_t>());
  if (Type == "boolean")
    return Value.is_boolean();
  if (Type == "array") {
    if (!Value.is_array() ||
        Value.size() > Rule.at("maxItems").get<std::size_t>())
      return false;
    return std::all_of(Value.begin(), Value.end(), [&](const Json &Item) {
      return fits(Item, Rule.at("items"));
    });
  }
  if (!Value.is_string())
    return false;
  const auto &Text = Value.get_ref<const std::string &>();
  const auto Length = characters(Text);
  if (Length < Rule.value("minLength", std::size_t(0)) ||
      Length > Rule.value("maxLength", std::size_t(4096)))
    return false;
  if (const auto Enum = Rule.find("enum");
      Enum != Rule.end() &&
      std::find(Enum->begin(), Enum->end(), Value) == Enum->end())
    return false;
  if (const auto Format = Rule.find("format");
      Format != Rule.end() && *Format == "address" && !namesAddress(Text))
    return false;
  return true;
}

//===----------------------------------------------------------------------===//
// Result shapes
//===----------------------------------------------------------------------===//

/// Whether a value is null, an empty string, or an empty array or object.
template <typename J> bool blank(const J &Value) {
  if (Value.is_string())
    return Value.template get_ref<const std::string &>().empty();
  return Value.is_null() ||
         ((Value.is_array() || Value.is_object()) && Value.empty());
}

/// A value as a line of text shows it: a string as itself.
std::string display(const Json &Value) {
  return Value.is_string() ? Value.get<std::string>() : Value.dump();
}

/// A string member, empty when absent or null.
std::string textOf(const Json &Object, const char *Key) {
  const auto &Value = member(Object, Key);
  return Value.is_null() ? std::string() : display(Value);
}

std::string rstrip(std::string Text) {
  while (!Text.empty() &&
         std::string_view(" \t\n\r\f\v").find(Text.back()) != std::string::npos)
    Text.pop_back();
  return Text;
}

std::string encode(const Ordered &Value) {
  return Value.dump(-1, ' ', false, Ordered::error_handler_t::replace);
}

std::string join(const std::vector<std::string> &Lines) {
  std::string Text;
  for (const auto &Line : Lines) {
    if (&Line != &Lines.front())
      Text += '\n';
    Text += Line;
  }
  return Text;
}

/// The value without null, empty-string, empty-list and empty-object
/// members.
Ordered pruned(const Ordered &Value) {
  if (Value.is_object()) {
    Ordered Kept = Ordered::object();
    for (const auto &[Key, Item] : Value.items())
      if (auto Child = pruned(Item); !blank(Child))
        Kept[Key] = std::move(Child);
    return Kept;
  }
  if (Value.is_array()) {
    Ordered Items = Ordered::array();
    for (const auto &Item : Value)
      Items.push_back(pruned(Item));
    return Items;
  }
  return Value;
}

void copy(const Json &Payload, Ordered &Out,
          std::initializer_list<const char *> Keys) {
  for (const char *Key : Keys)
    if (Payload.contains(Key))
      Out[Key] = Ordered(Payload[Key]);
}

std::string code(const Ordered &Header, const std::string &Text) {
  return encode(Header) + "\n" + Text;
}

template <typename Row>
Ordered table(const Json &Payload, Ordered Out,
              std::initializer_list<const char *> Columns, Row MakeRow) {
  copy(Payload, Out, {"total", "offset", "next_offset", "complete"});
  Out["columns"] = Ordered::array();
  for (const char *Column : Columns)
    Out["columns"].push_back(Column);
  Ordered Rows = Ordered::array();
  if (const auto &Items = member(Payload, "items"); Items.is_array())
    for (const auto &Item : Items)
      if (Item.is_object())
        Rows.push_back(MakeRow(Item));
  Out["rows"] = std::move(Rows);
  return Out;
}

Ordered column(const Json &Item, const char *Key) {
  return Ordered(member(Item, Key));
}

// Project bookkeeping of the workbench in a metadata payload.
bool bookkeeping(const std::string &Key) {
  return Key == "analysis_state" || Key == "dirty" || Key == "read_only" ||
         Key == "load_options" || Key == "project";
}

Ordered metadata(const Json &Payload, Ordered Out) {
  // The input's hash, which the worker names beside the project's and an
  // older worker only with it, shows once, last.
  const auto &Hash =
      truthy(member(Payload, "sha256"))
          ? member(Payload, "sha256")
          : member(member(member(Payload, "project"), "input"), "sha256");
  Ordered Kept = Ordered::object();
  for (const auto &[Key, Value] : Payload.items())
    if (!bookkeeping(Key) && Key != "sha256")
      Kept[Key] = Ordered(Value);
  const auto Shown = pruned(Kept);
  for (const auto &[Key, Value] : Shown.items())
    Out[Key] = Value;
  if (truthy(Hash))
    Out["sha256"] = Ordered(Hash);
  return Out;
}

Ordered functions(const Json &Payload, Ordered Out) {
  return table(Payload, std::move(Out), {"address", "name", "size", "flags"},
               [](const Json &Item) {
                 std::string Flags;
                 for (const char *Flag : {"library", "thunk", "exported"})
                   if (truthy(member(Item, Flag)))
                     Flags += (Flags.empty() ? "" : ",") + std::string(Flag);
                 return Ordered::array({column(Item, "address"),
                                        column(Item, "name"),
                                        column(Item, "size"), Flags});
               });
}

Ordered strings(const Json &Payload, Ordered Out) {
  return table(Payload, std::move(Out), {"address", "type", "length", "text"},
               [](const Json &Item) {
                 return Ordered::array(
                     {column(Item, "address"), column(Item, "type"),
                      column(Item, "length"), column(Item, "text")});
               });
}

Ordered xrefs(const Json &Payload, Ordered Out, const std::string &Direction) {
  const char *Other = Direction == "from" ? "to" : "from";
  return table(Payload, std::move(Out), {Other, "kind", "location"},
               [&](const Json &Item) {
                 const auto &Text = member(Item, "text");
                 return Ordered::array(
                     {Ordered(Item.contains(Other) ? Item[Other]
                                                   : member(Item, "address")),
                      column(Item, "kind"),
                      Ordered(truthy(Text) ? Text : member(Item, "function"))});
               });
}

Ordered imports(const Json &Payload, Ordered Out) {
  // Names resolve wherever a tool takes an address, so a page groups them
  // by module rather than repeating module and slot on every row.
  copy(Payload, Out, {"total", "offset", "next_offset", "complete"});
  Ordered Modules = Ordered::object();
  if (const auto &Items = member(Payload, "items"); Items.is_array())
    for (const auto &Item : Items)
      if (Item.is_object()) {
        const auto &Module = member(Item, "module");
        auto &Names = Modules[truthy(Module) ? display(Module) : std::string()];
        if (Names.is_null())
          Names = Ordered::array();
        Names.push_back(column(Item, "name"));
      }
  Out["modules"] = std::move(Modules);
  return Out;
}

/// A display preview of at most \p Limit UTF-8 bytes, as the worker cuts
/// string texts.
Json cut(const Json &Text, std::size_t Limit = DefaultStringTextLimit) {
  if (!Text.is_string() || Text.get_ref<const std::string &>().size() <= Limit)
    return Text;
  const auto &Whole = Text.get_ref<const std::string &>();
  std::size_t End = Limit;
  // A character the limit splits is left out whole.
  while (End > 0 && (static_cast<unsigned char>(Whole[End]) & 0xC0) == 0x80)
    --End;
  return Whole.substr(0, End) + "\xE2\x80\xA6";
}

Ordered stringReferences(const Json &Payload, Ordered Out) {
  return table(Payload, std::move(Out),
               {"address", "function", "kind", "text", "via"},
               [](const Json &Item) {
                 return Ordered::array(
                     {column(Item, "address"), column(Item, "function"),
                      column(Item, "kind"), Ordered(cut(member(Item, "text"))),
                      column(Item, "via")});
               });
}

std::string disassembly(const Json &Payload, Ordered Out) {
  std::vector<std::string> Lines;
  if (const auto &Items = member(Payload, "items"); Items.is_array())
    for (const auto &Item : Items) {
      auto Line =
          rstrip(textOf(Item, "address") + "  " + textOf(Item, "mnemonic") +
                 " " + textOf(Item, "operands"));
      if (const auto &Comment = member(Item, "comment"); truthy(Comment))
        Line += "  ; " + display(Comment);
      Lines.push_back(std::move(Line));
    }
  copy(Payload, Out, {"address", "next_address", "complete"});
  return code(Out, join(Lines));
}

std::string decompilation(const Json &Payload, Ordered Out, bool Exact) {
  Out["address"] = Ordered(member(Payload, "address"));
  const auto &Dialect = member(Payload, "dialect");
  Out["representation"] =
      Ordered(truthy(Dialect) ? Dialect : member(Payload, "representation"));
  for (const char *Key : {"prelude_lines", "offset", "total_lines",
                          "next_offset", "complete", "unread"}) {
    const auto &Value = member(Payload, Key);
    if (!(Value.is_null() || (Value.is_array() && Value.empty()) ||
          (Value.is_string() && Value.get_ref<const std::string &>().empty())))
      Out[Key] = Ordered(Value);
  }
  const auto &Body = member(Payload, "text");
  std::string Text = Body.is_string() ? Body.get<std::string>() : "";
  // LLVM-C spells the same fixed-width types.
  if (!Exact &&
      (Out["representation"] == "c" || Out["representation"] == "llvmc") &&
      !Text.empty()) {
    // The same C in fewer tokens; the legend lists every abbreviation.
    const auto &Function = member(Payload, "function_text");
    auto View = readingView(Text, Function.is_string()
                                      ? Function.get_ref<const std::string &>()
                                      : std::string_view());
    Text = std::move(View.Text);
    if (!View.Legend.empty())
      Out["legend"] = View.Legend;
  }
  return code(Out, Text);
}

Ordered cfg(const Json &Payload, Ordered Out) {
  copy(Payload, Out, {"name", "entry", "complete"});
  Ordered Nodes = Ordered::array(), Edges = Ordered::array();
  if (const auto &All = member(Payload, "nodes"); All.is_array())
    for (const auto &Node : All) {
      Ordered Lines = Ordered::array();
      if (const auto &Disassembly = member(Node, "disasm");
          Disassembly.is_array())
        for (const auto &Line : Disassembly)
          Lines.push_back(Line.is_string() ? Ordered(rstrip(Line))
                                           : Ordered(Line));
      Nodes.push_back(Ordered::array(
          {column(Node, "id"), column(Node, "start"), column(Node, "end"),
           column(Node, "insn_count"), std::move(Lines)}));
    }
  if (const auto &All = member(Payload, "edges"); All.is_array())
    for (const auto &Edge : All)
      Edges.push_back(Ordered::array(
          {column(Edge, "from"), column(Edge, "to"), column(Edge, "type")}));
  Out["nodes"] = std::move(Nodes);
  Out["edges"] = std::move(Edges);
  Out["node_columns"] =
      Ordered::array({"id", "start", "end", "insn_count", "disasm"});
  return Out;
}

Ordered search(const Json &Payload, Ordered Out, const std::string &Kind) {
  copy(Payload, Out, {"complete", "next_address", "coverage"});
  // A text match shows the text it is in; a code match, its function, and
  // for an immediate its instruction.
  const bool Text = Kind == "text", Immediate = Kind == "immediate";
  Out["columns"] = Text ? Ordered::array({"address", "context"})
                   : Immediate
                       ? Ordered::array({"address", "function", "instruction"})
                       : Ordered::array({"address", "function"});
  Ordered Rows = Ordered::array();
  if (const auto &Items = member(Payload, "items"); Items.is_array())
    for (const auto &Item : Items) {
      Ordered Row = Ordered::array({column(Item, "address")});
      for (const char *Field : Text ? std::vector{"context"}
                               : Immediate
                                   ? std::vector{"function", "instruction"}
                                   : std::vector{"function"})
        Row.push_back(column(Item, Field));
      Rows.push_back(std::move(Row));
    }
  Out["rows"] = std::move(Rows);
  return Out;
}

std::string lower(std::string Text) {
  std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char C) {
    return static_cast<char>(C >= 'A' && C <= 'Z' ? C - 'A' + 'a' : C);
  });
  return Text;
}

/// How a path or tree names each node: a function by name and address, an
/// import by name; an automatic sub_ name already spells its address.
class Labels {
  std::map<std::string, std::string> Named;

public:
  explicit Labels(const Json &Payload) {
    const auto &Nodes = member(Payload, "nodes");
    if (!Nodes.is_array())
      return;
    for (const auto &Node : Nodes) {
      const auto Address = textOf(Node, "address");
      const auto &Given = member(Node, "name");
      const auto Name = truthy(Given) ? display(Given) : Address;
      if (member(Node, "kind") == "import")
        Named[Address] = Name + "@import";
      else if (lower(Name) ==
               "sub_" + lower(Address.size() > 2 ? Address.substr(2) : ""))
        Named[Address] = Name;
      else
        Named[Address] = Name + "@" + Address;
    }
  }
  std::string operator()(const Json &Address) const {
    if (Address.is_string())
      if (const auto It = Named.find(Address.get<std::string>());
          It != Named.end())
        return It->second;
    return display(Address);
  }
};

std::string callPaths(const Json &Payload, Ordered Out) {
  const Labels Label(Payload);
  Out["max_depth"] = Ordered(member(Payload, "max_depth"));
  std::vector<std::string> Lines;
  if (const auto &Segments = member(Payload, "segments"); Segments.is_array())
    for (const auto &Segment : Segments) {
      const auto Head = Label(member(Segment, "from")) + " -> " +
                        Label(member(Segment, "to"));
      const auto &Shortest = member(Segment, "shortest");
      if (Shortest.is_null()) {
        Lines.push_back(Head + ": no path either way");
        continue;
      }
      auto Line = Head + ": shortest " + display(Shortest) + ", " +
                  display(member(Segment, "node_count")) + " nodes on paths";
      if (truthy(member(Segment, "reversed")))
        Line += ", only reversed";
      if (Segment.contains("complete") && !truthy(Segment["complete"]))
        Line += ", more paths not listed";
      Lines.push_back(std::move(Line));
      if (const auto &Paths = member(Segment, "paths"); Paths.is_array())
        for (const auto &Path : Paths) {
          std::string Steps;
          if (Path.is_array())
            for (const auto &Step : Path)
              Steps += (Steps.empty() ? "" : " -> ") + Label(Step);
          Lines.push_back("  " + Steps);
        }
    }
  return code(Out, join(Lines));
}

std::string callTree(const Json &Payload, Ordered Out) {
  const Labels Label(Payload);
  const bool Callees = member(Payload, "direction") == "callees";
  copy(Payload, Out, {"direction", "depth_limited", "complete"});
  const auto &Nodes = member(Payload, "nodes");
  std::map<std::string, std::int64_t> Depth;
  if (Nodes.is_array())
    for (const auto &Node : Nodes) {
      const auto &Level = member(Node, "depth");
      Depth[textOf(Node, "address")] =
          Level.is_number_integer() ? Level.get<std::int64_t>() : 0;
    }
  std::map<std::string, std::vector<std::string>> Children;
  std::set<std::string> Placed;
  if (const auto &Edges = member(Payload, "edges"); Edges.is_array())
    for (const auto &Edge : Edges) {
      if (!Edge.is_array() || Edge.size() < 2 || !Edge[0].is_string() ||
          !Edge[1].is_string())
        continue;
      const auto &Near = Edge[Callees ? 0 : 1].get_ref<const std::string &>();
      const auto &Far = Edge[Callees ? 1 : 0].get_ref<const std::string &>();
      const auto NearDepth = Depth.find(Near), FarDepth = Depth.find(Far);
      if (NearDepth != Depth.end() && FarDepth != Depth.end() &&
          FarDepth->second == NearDepth->second + 1 &&
          Placed.insert(Far).second)
        Children[Near].push_back(Far);
    }
  std::vector<std::string> Lines;
  // Each level is one deeper than its parent's, so the walk ends.
  auto Walk = [&](auto &Self, const std::string &At,
                  std::size_t Indent) -> void {
    Lines.push_back(std::string(2 * Indent, ' ') + Label(Json(At)));
    if (const auto It = Children.find(At); It != Children.end())
      for (const auto &Child : It->second)
        Self(Self, Child, Indent + 1);
  };
  if (Nodes.is_array() && !Nodes.empty())
    Walk(Walk, textOf(Nodes[0], "address"), 0);
  return code(Out, join(Lines));
}
} // namespace

const Json &member(const Json &Object, const char *Key) {
  static const Json Absent;
  if (!Object.is_object())
    return Absent;
  const auto It = Object.find(Key);
  return It == Object.end() ? Absent : *It;
}

bool truthy(const Json &Value) {
  if (Value.is_boolean())
    return Value.get<bool>();
  if (Value.is_number())
    return Value.get<double>() != 0;
  return !blank(Value);
}

const std::vector<Tool> &projectTools() {
  static const auto Tools = catalog();
  return Tools;
}

const std::vector<Tool> &workbenchTools() {
  static const auto Tools = workbenchCatalog();
  return Tools;
}

void validate(const Json &Arguments, const Json &Schema) {
  if (!Arguments.is_object())
    throw RpcError{-32602, "arguments must be an object"};
  const auto &Properties = Schema.at("properties");
  for (const auto &Item : Arguments.items())
    if (!Properties.contains(Item.key()))
      throw RpcError{-32602, "Unknown or missing tool argument"};
  if (const auto Required = Schema.find("required"); Required != Schema.end())
    for (const auto &Name : *Required)
      if (!Arguments.contains(Name.get_ref<const std::string &>()))
        throw RpcError{-32602, "Unknown or missing tool argument"};
  for (const auto &Item : Arguments.items())
    if (!fits(Item.value(), Properties.at(Item.key())))
      throw RpcError{-32602, "Invalid argument: " + Item.key()};
}

bool hexAddress(std::string_view Text) {
  if (Text.size() < 3 || Text.size() > 18 || Text.substr(0, 2) != "0x")
    return false;
  return std::all_of(Text.begin() + 2, Text.end(), [](char C) {
    return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') ||
           (C >= 'A' && C <= 'F');
  });
}

std::pair<std::string, bool> compactResult(const std::string &Operation,
                                           const Json &Arguments,
                                           const Json &Response) {
  if (!Response.is_object())
    return {Response.dump(-1, ' ', false, Json::error_handler_t::replace),
            true};
  Ordered Out{{"revision", Ordered(member(Response, "revision"))}};
  const auto &Status = member(Response, "status");
  if (Status != "ok" && Status != "success") {
    Out["status"] = Ordered(Status);
    Out["error"] = Ordered(member(Response, "error"));
    return {encode(pruned(Out)), true};
  }
  Json Payload = member(Response, "payload");
  if (!Payload.is_object())
    Payload = Payload.is_null() ? Json::object()
                                : Json{{"value", std::move(Payload)}};
  if (Operation == "metadata")
    return {encode(metadata(Payload, std::move(Out))), false};
  if (Operation == "functions")
    return {encode(functions(Payload, std::move(Out))), false};
  if (Operation == "strings")
    return {encode(strings(Payload, std::move(Out))), false};
  if (Operation == "imports")
    return {encode(imports(Payload, std::move(Out))), false};
  if (Operation == "string_references")
    return {encode(stringReferences(Payload, std::move(Out))), false};
  if (Operation == "xrefs") {
    const auto &Direction = member(Arguments, "direction");
    return {encode(xrefs(Payload, std::move(Out),
                         Direction.is_string() ? Direction.get<std::string>()
                                               : "to")),
            false};
  }
  if (Operation == "cfg")
    return {encode(cfg(Payload, std::move(Out))), false};
  if (Operation == "disasm")
    return {disassembly(Payload, std::move(Out)), false};
  if (Operation == "decompile")
    return {decompilation(Payload, std::move(Out),
                          truthy(member(Arguments, "exact"))),
            false};
  if (Operation == "call_paths")
    return {callPaths(Payload, std::move(Out)), false};
  if (Operation == "call_tree")
    return {callTree(Payload, std::move(Out)), false};
  if (Operation == "search") {
    const auto &Kind = member(Arguments, "kind");
    return {encode(search(Payload, std::move(Out),
                          Kind.is_string() ? Kind.get<std::string>() : "")),
            false};
  }
  // Worker transport and session fields an agent never acts on.
  for (const auto &[Key, Value] : Payload.items())
    if (Key != "type" && Key != "protocol_major" && Key != "request_id" &&
        Key != "operation" && Key != "analysis_state" && Key != "project_id")
      Out[Key] = Ordered(Value);
  return {encode(Out), false};
}

} // namespace neverd::binmcp
