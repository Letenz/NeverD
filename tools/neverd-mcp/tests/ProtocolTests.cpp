//===- ProtocolTests.cpp - MCP lifecycle, tools and resources -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The server over a backend that records each query and answers what the
/// case sets; no engine runs.
///
//===----------------------------------------------------------------------===//

#include "Server.h"
#include "StdioTransport.h"
#include "Testing.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>

using namespace neverd::binmcp;
using neverd::binmcp::test::require;
namespace transport = neverd::transport;

namespace {
struct FakeBackend final : Backend {
  bool Attached = false;
  struct Call {
    std::string Operation;
    Json Payload;
    std::optional<std::string> Revision;
  };
  std::vector<Call> Calls;
  Json Response{{"type", "response"},
                {"status", "ok"},
                {"revision", "42"},
                {"payload", {{"items", Json::array()}}}};
  /// Answers in turn, before Response.
  std::deque<Json> Replies;
  std::atomic<unsigned> IdleSteps = 0, IdleWork = 0;

  explicit FakeBackend(bool Attached = false) : Attached(Attached) {}
  Json query(const std::string &Operation, const Json &Payload,
             const std::optional<std::string> &Revision) override {
    Calls.push_back({Operation, Payload, Revision});
    if (Replies.empty())
      return Response;
    auto Next = std::move(Replies.front());
    Replies.pop_front();
    return Next;
  }
  bool attached() const override { return Attached; }
  bool hasIdleWork() const override { return IdleSteps < IdleWork; }
  bool idleStep() override {
    ++IdleSteps;
    return true;
  }
};

Json request(const char *Method, Json Params = nullptr, Json ID = 1) {
  Json Message{{"jsonrpc", "2.0"}, {"method", Method}};
  if (!ID.is_null())
    Message["id"] = std::move(ID);
  if (!Params.is_null())
    Message["params"] = std::move(Params);
  return Message;
}

Json initialize() {
  return request("initialize",
                 {{"protocolVersion", std::string(ProtocolVersion)},
                  {"capabilities", Json::object()},
                  {"clientInfo", {{"name", "test"}, {"version", "1"}}}});
}

void ready(Server &Service) {
  require(Service.handle(initialize())["protocolVersion"] ==
              std::string(ProtocolVersion),
          "initialize answers the protocol version");
  Service.handle(request("notifications/initialized", nullptr, nullptr));
}

int rpcError(Server &Service, const Json &Message) {
  try {
    Service.handle(Message);
  } catch (const RpcError &Error) {
    return Error.Code;
  }
  throw std::runtime_error("expected an RPC error for " + Message.dump());
}

Json call(Server &Service, const char *Tool, Json Arguments = Json::object()) {
  return Service.handle(request(
      "tools/call", {{"name", Tool}, {"arguments", std::move(Arguments)}}));
}

std::string text(const Json &Result, std::size_t Index = 0) {
  return Result.at("content").at(Index).at("text").get<std::string>();
}

/// The header line of a code result, and the code after it.
std::pair<Json, std::string> code(const Json &Result) {
  const auto Whole = text(Result);
  const auto Newline = Whole.find('\n');
  return {transport::parseJson(Whole.substr(0, Newline)),
          Newline == std::string::npos ? "" : Whole.substr(Newline + 1)};
}

/// The lines serve() writes for \p Input, once it has read all of it.
std::vector<Json> served(Server &Service, Backend &Session, std::string Input) {
  std::size_t At = 0;
  std::string Output;
  const auto Status = serve(
      Service, Session,
      [&](char *Into, std::size_t Size) {
        const auto Count = std::min(Size, Input.size() - At);
        std::copy_n(Input.data() + At, Count, Into);
        At += Count;
        return Count;
      },
      [&](std::string_view Line) {
        Output += Line;
        return true;
      });
  require(Status == 0, "serve ends when its input does");
  std::vector<Json> Lines;
  for (std::size_t Start = 0; Start < Output.size();) {
    const auto End = Output.find('\n', Start);
    Lines.push_back(transport::parseJson(Output.substr(Start, End - Start)));
    Start = End + 1;
  }
  return Lines;
}

std::string line(const Json &Message) { return Message.dump() + "\n"; }
} // namespace

MCP_TEST(HandshakeAndLifecycle) {
  FakeBackend Session;
  Server Service(Session);
  require(rpcError(Service, request("tools/list")) == -32002,
          "tools wait for initialization");
  ready(Service);
  const auto Tools = Service.handle(request("tools/list"))["tools"];
  require(Tools.size() == 13, "thirteen tools without a workbench");
  for (const auto &Listed : Tools) {
    require(!Listed.contains("operation"), "the operation is not listed");
    require(Listed["annotations"]["readOnlyHint"] == true, "tools only read");
  }
  require(rpcError(Service, initialize()) == -32600, "one initialization");
}

MCP_TEST(ACurrentClientGetsItsProtocolRevision) {
  FakeBackend Session;
  Server Service(Session);
  auto Message = initialize();
  Message["params"]["protocolVersion"] = "2025-06-18";
  require(Service.handle(Message)["protocolVersion"] == "2025-06-18",
          "2025-06-18 is answered in kind");
}

MCP_TEST(OnlyPagesTakeARevisionGuard) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  // A copy: a loop over a member of handle()'s temporary would read it after
  // the temporary is destroyed.
  const auto Tools = Service.handle(request("tools/list"))["tools"];
  for (const auto &Listed : Tools) {
    const auto &Properties = Listed["inputSchema"]["properties"];
    require(Properties.contains("expected_revision") ==
                Properties.contains("offset"),
            Listed["name"].get<std::string>());
  }
}

MCP_TEST(AddressAndRevisionRemainStrings) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  const auto Result = call(Service, "list_xrefs",
                           {{"address", "0xffffffffffffffff"},
                            {"limit", 2},
                            {"expected_revision", "18446744073709551615"}});
  require(Result["isError"] == false, "the call succeeds");
  require(Session.Calls.size() == 1 && Session.Calls[0].Operation == "xrefs" &&
              Session.Calls[0].Payload ==
                  Json{{"address", "0xffffffffffffffff"}, {"limit", 2}} &&
              Session.Calls[0].Revision == "18446744073709551615",
          "the worker gets the address and revision as given");
}

MCP_TEST(ValidationRejectsPathChangesAndUnboundedPages) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  const std::vector<std::pair<const char *, Json>> Cases{
      {"list_functions", {{"limit", 513}}},
      {"list_functions", {{"offset", -1}}},
      {"list_functions", {{"limit", true}}},
      {"list_functions", {{"limit", 1.5}}},
      {"project_metadata", {{"path", "/new/input"}}},
      {"read_bytes", {{"address", "0x10000000000000000"}}},
      {"read_bytes", {{"address", 9007199254740993ULL}}},
      {"read_bytes", {{"address", "0X10"}}},
      {"read_bytes", {{"address", "name\nwith a line"}}},
      {"read_decompilation", {{"address", "0x1"}, {"representation", "shell"}}},
      {"open", {{"path", "/new/input"}}},
      {"rename", {{"name", "new"}}},
      {"read_disassembly", Json::array()}};
  for (const auto &[Tool, Arguments] : Cases)
    require(rpcError(Service, request("tools/call",
                                      {{"name", Tool},
                                       {"arguments", Arguments}})) == -32602,
            std::string(Tool) + " " + Arguments.dump());
  require(Session.Calls.empty(), "no rejected call reaches the worker");
}

MCP_TEST(GuiResourcesAndNavigationRequireAttachment) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  rpcError(Service,
           request("resources/read", {{"uri", "neverd://gui/selection"}}));
  FakeBackend Workbench(true);
  Server Attached(Workbench);
  ready(Attached);
  const auto Result = call(Attached, "navigate_gui", {{"address", "0x123"}});
  require(Result["isError"] == false, "navigation runs");
  require(Workbench.Calls.at(0).Operation == "navigate", "navigate runs");
  rpcError(Attached,
           request("resources/read",
                   {{"uri", "neverd://project/navigate?address=0x1"}}));
  const auto Tools = Attached.handle(request("tools/list"))["tools"];
  require(Tools.size() == 16, "a workbench adds three tools");
  require(Tools.back()["annotations"]["readOnlyHint"] == false,
          "navigation is no read");
}

MCP_TEST(ResourcesAreScopedAndPaginated) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Service.handle(
      request("resources/read",
              {{"uri", "neverd://project/"
                       "functions?offset=20&limit=10&expected_revision=42"}}));
  require(Session.Calls.back().Operation == "functions" &&
              Session.Calls.back().Payload ==
                  Json{{"offset", 20}, {"limit", 10}} &&
              Session.Calls.back().Revision == "42",
          "a resource page reaches the worker");
  for (const char *URI :
       {"file:///etc/passwd", "https://example.com/", "neverd://other/metadata",
        "neverd://project/functions?limit=1&limit=2",
        "neverd://user@project/metadata", "neverd://project:1/metadata",
        "neverd://project/functions?limit=x"})
    rpcError(Service, request("resources/read", {{"uri", URI}}));
}

MCP_TEST(ErrorsAndBudgetAreNotSuccessfulEmptyResults) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {{"status", "error"},
                      {"revision", "42"},
                      {"error", {{"code", "stale_revision"}}}};
  require(call(Service, "project_metadata")["isError"] == true,
          "an error is an error");
  Session.Response = {
      {"status", "ok"},
      {"revision", "42"},
      {"payload", {{"text", std::string(MaxCachedResults, 'x')}}}};
  const auto Result = call(Service, "project_metadata");
  require(Result["isError"] == true, "an oversized result is an error");
  require(transport::parseJson(text(Result))["status"] == "budget_exceeded",
          "an oversized result says so");
}

MCP_TEST(LargeResultsReturnBoundedRevisionStableResourcePages) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  std::string Large;
  for (std::size_t I = 0; I < MaxResult; ++I)
    Large += "\xE4\xB8\xAD\xE6\x96\x87\n";
  const Json Expected{{"status", "ok"},
                      {"revision", "42"},
                      {"project_id", "project-1"},
                      {"payload", {{"text", Large}}}};
  Session.Response = Expected;
  const auto Result = call(Service, "project_metadata");
  require(Result["isError"] == false, "a large result is no error");
  require(Result["content"][0]["type"] == "resource_link",
          "a large result is linked");
  const auto URI = Result["content"][0]["uri"].get<std::string>();
  Session.Response = {{"status", "ok"}, {"revision", "43"}};
  std::string Whole;
  for (std::size_t Offset = 0;;) {
    const auto Read = Service.handle(request(
        "resources/read", {{"uri", URI + "?offset=" + std::to_string(Offset) +
                                       "&limit=65536&expected_revision=42"}}));
    const auto Page =
        transport::parseJson(Read["contents"][0]["text"].get<std::string>());
    require(Page["revision"] == "42", "pages keep their revision");
    Whole += Page["text"].get<std::string>();
    if (Page["complete"] == true)
      break;
    Offset = Page["next_offset"].get<std::size_t>();
  }
  require(Whole == compactResult("metadata", Json::object(), Expected).first,
          "the pages make up the result");
  require(Session.Calls.size() == 1, "pages are read from the snapshot");
  rpcError(Service,
           request("resources/read", {{"uri", URI + "?expected_revision=43"}}));
}

MCP_TEST(ToolsReturnWhatAnAgentReads) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {
      {"type", "response"},
      {"protocol_major", 1},
      {"request_id", "9"},
      {"operation", "decompile"},
      {"analysis_state", "complete"},
      {"project_id", "project-1"},
      {"revision", "7"},
      {"status", "ok"},
      {"payload",
       {{"address", "0x401000"},
        {"representation", "c"},
        {"dialect", "c"},
        {"text", "int main(void) {\n    return 0;\n}\n"},
        {"offset", 0},
        {"total_lines", 3},
        {"next_offset", nullptr},
        {"complete", true},
        {"rows", Json::array({{{"line", 0}, {"address", "0x401000"}}})},
        {"source_variables", Json::array({{{"name", "v"}}})},
        {"source_calls", Json::array()}}}};
  auto Result = call(Service, "read_decompilation", {{"address", "0x401000"}});
  require(Result["isError"] == false && !Result.contains("structuredContent"),
          "a code result is text alone");
  auto [Header, Code] = code(Result);
  require(Header == Json{{"revision", "7"},
                         {"address", "0x401000"},
                         {"representation", "c"},
                         {"offset", 0},
                         {"total_lines", 3},
                         {"complete", true}},
          "the header: " + Header.dump());
  require(Code == "int main(void) {\n    return 0;\n}\n", "the code");
  // exact=true asks for the engine's spelling, which is the same here.
  Result = call(Service, "read_decompilation",
                {{"address", "0x401000"}, {"exact", true}});
  require(code(Result).second == Code, "the exact code");
  require(!Session.Calls.back().Payload.contains("exact"),
          "exact is the server's");
  // The complete worker response stays available as a resource.
  const auto Contents = Service.handle(
      request("resources/read",
              {{"uri", "neverd://project/decompile?address=0x401000"}}));
  require(transport::parseJson(
              Contents["contents"][0]["text"].get<std::string>())["payload"]
              .contains("rows"),
          "a resource keeps the rows");
}

MCP_TEST(TablesAreColumnsAndRows) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {
      {"status", "ok"},
      {"revision", "3"},
      {"payload",
       {{"items", Json::array({{{"addr", "0x1000"},
                                {"address", "0x1000"},
                                {"name", "main"},
                                {"display_name", "main"},
                                {"engine_name", "sub_1000"},
                                {"size", 42},
                                {"library", false},
                                {"thunk", true},
                                {"exported", true},
                                {"library_annotations", Json::array()}}})},
        {"total", 1},
        {"offset", 0},
        {"next_offset", nullptr},
        {"complete", true}}}};
  auto Result = call(Service, "list_functions", {{"limit", 1}});
  require(text(Result) ==
              R"({"revision":"3","total":1,"offset":0,"next_offset":null,)"
              R"("complete":true,"columns":["address","name","size","flags"],)"
              R"("rows":[["0x1000","main",42,"thunk,exported"]]})",
          "a function page: " + text(Result));
  call(Service, "list_strings", {{"filter", "http"}});
  require(Session.Calls.back().Payload ==
              Json{{"filter", "http"}, {"text_limit", DefaultStringTextLimit}},
          "strings are cut by default");
  call(Service, "find_string_references", {{"filter", "passw"}});
  require(Session.Calls.back().Operation == "string_references" &&
              Session.Calls.back().Payload == Json{{"filter", "passw"}},
          "string references take the filter");
  Session.Response = {{"status", "ok"},
                      {"revision", "3"},
                      {"payload",
                       {{"total", 3},
                        {"offset", 0},
                        {"next_offset", nullptr},
                        {"complete", true},
                        {"items", Json::array({{{"address", "0x10"},
                                                {"module", "ws2_32.dll"},
                                                {"name", "connect"}},
                                               {{"address", "0x18"},
                                                {"module", "kernel32.dll"},
                                                {"name", "CreateFileW"}},
                                               {{"address", "0x20"},
                                                {"module", "ws2_32.dll"},
                                                {"name", "send"}}})}}}};
  Result = call(Service, "list_imports");
  require(text(Result) ==
              R"({"revision":"3","total":3,"offset":0,"next_offset":null,)"
              R"("complete":true,"modules":{"ws2_32.dll":["connect","send"],)"
              R"("kernel32.dll":["CreateFileW"]}})",
          "imports group by module: " + text(Result));
}

MCP_TEST(StringReferencesCutLongTextsOnACharacter) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  std::string Long(199, 'a');
  Long += "\xE4\xB8\xAD";
  Session.Response = {{"status", "ok"},
                      {"revision", "1"},
                      {"payload",
                       {{"items", Json::array({{{"address", "0x10"},
                                                {"function", "main"},
                                                {"kind", "read"},
                                                {"text", Long},
                                                {"via", nullptr}}})}}}};
  const auto Rows = transport::parseJson(
      text(call(Service, "find_string_references")))["rows"];
  require(Rows[0][3] == std::string(199, 'a') + "\xE2\x80\xA6",
          "a character the limit splits is left out: " + Rows.dump());
}

MCP_TEST(NamesResolveToAddressesFirst) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Replies = {
      {{"status", "ok"},
       {"revision", "5"},
       {"payload", {{"address", "0x2a90"}, {"function_address", "0x2a90"}}}},
      {{"status", "ok"},
       {"revision", "5"},
       {"payload", {{"text", "int main(void);\n"}}}}};
  const auto Result =
      call(Service, "read_decompilation", {{"address", "main"}});
  require(Result["isError"] == false, "the call succeeds");
  require(Session.Calls.size() == 2 &&
              Session.Calls[0].Operation == "resolve" &&
              Session.Calls[0].Payload == Json{{"query", "main"}} &&
              Session.Calls[1].Operation == "decompile" &&
              Session.Calls[1].Payload == Json{{"address", "0x2a90"}},
          "a name resolves first");
  // A name the worker does not know is reported, and nothing else runs.
  Session.Calls.clear();
  Session.Replies = {{{"status", "error"},
                      {"revision", "5"},
                      {"error", {{"code", "not_found"}, {"message", "No x"}}}}};
  require(call(Service, "read_cfg", {{"address", "x"}})["isError"] == true &&
              Session.Calls.size() == 1,
          "an unknown name is an error");
}

MCP_TEST(GraphToolsTakeNamesAndWaypoints) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {
      {"status", "ok"},
      {"revision", "1"},
      {"payload",
       {{"segments",
         Json::array({{{"from", "0x10"},
                       {"to", "0x30"},
                       {"shortest", 2},
                       {"node_count", 3},
                       {"complete", true},
                       {"paths", Json::array({Json::array(
                                     {"0x10", "0x20", "0x30"})})}}})},
        {"unconnected", 0},
        {"max_depth", 16},
        {"nodes",
         Json::array(
             {{{"address", "0x10"}, {"name", "main"}, {"kind", "function"}},
              {{"address", "0x20"}, {"name", "sub_20"}, {"kind", "function"}},
              {{"address", "0x30"},
               {"name", "connect"},
               {"kind", "import"}}})}}}};
  const auto Result = call(Service, "find_call_paths",
                           {{"from", "main"},
                            {"via", Json::array({"sub_20"})},
                            {"to", "connect"},
                            {"shortest_only", true}});
  require(
      Session.Calls.back().Operation == "call_paths" &&
          Session.Calls.back().Payload ==
              Json{{"waypoints", Json::array({"main", "sub_20", "connect"})},
                   {"shortest_only", true}},
      "waypoints run from, via, to");
  require(code(Result).second ==
              "main@0x10 -> connect@import: shortest 2, 3 nodes on paths\n"
              "  main@0x10 -> sub_20 -> connect@import",
          "paths read as lines: " + code(Result).second);
  call(Service, "list_call_tree", {{"function", "connect"}, {"callers", true}});
  require(Session.Calls.back().Operation == "call_tree" &&
              Session.Calls.back().Payload ==
                  Json{{"root", "connect"}, {"direction", "callers"}},
          "a tree of callers");
  for (const auto &Arguments :
       {Json{{"from", "main"}, {"to", ""}},
        Json{{"from", "main"}, {"to", "x"}, {"via", "y"}},
        Json{{"from", "main"}, {"to", "x"}, {"shortest_only", 1}}})
    require(rpcError(Service, request("tools/call",
                                      {{"name", "find_call_paths"},
                                       {"arguments", Arguments}})) == -32602,
            Arguments.dump());
}

MCP_TEST(CallTreesIndentEachLevel) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {
      {"status", "ok"},
      {"revision", "1"},
      {"payload",
       {{"direction", "callees"},
        {"depth_limited", false},
        {"complete", true},
        {"nodes", Json::array({{{"address", "0x10"},
                                {"name", "main"},
                                {"kind", "function"},
                                {"depth", 0}},
                               {{"address", "0x20"},
                                {"name", "sub_20"},
                                {"kind", "function"},
                                {"depth", 1}},
                               {{"address", "0x30"},
                                {"name", "connect"},
                                {"kind", "import"},
                                {"depth", 2}}})},
        {"edges", Json::array({Json::array({"0x10", "0x20"}),
                               Json::array({"0x20", "0x30"}),
                               Json::array({"0x10", "0x30"})})}}}};
  const auto [Header, Tree] =
      code(call(Service, "list_call_tree", {{"function", "main"}}));
  require(Tree == "main@0x10\n  sub_20\n    connect@import",
          "the tree: " + Tree);
  require(Header == Json{{"revision", "1"},
                         {"direction", "callees"},
                         {"depth_limited", false},
                         {"complete", true}},
          "the header: " + Header.dump());
}

MCP_TEST(SourcePagesStartAtTheDefinition) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  const Json First{{"status", "ok"},
                   {"revision", "2"},
                   {"payload",
                    {{"address", "0x10"},
                     {"dialect", "c"},
                     {"text", "#include <stdint.h>\n\nint f(void) {\n"},
                     {"offset", 0},
                     {"total_lines", 5},
                     {"next_offset", 3},
                     {"complete", false},
                     {"prelude", {{"lines", 2}, {"end_byte", 21}}}}}};
  Session.Replies = {First,
                     {{"status", "ok"},
                      {"revision", "2"},
                      {"payload",
                       {{"address", "0x10"},
                        {"dialect", "c"},
                        {"text", "int f(void) {\n    return 0;\n}\n"},
                        {"offset", 2},
                        {"total_lines", 5},
                        {"next_offset", nullptr},
                        {"complete", true}}}}};
  auto [Header, Code] =
      code(call(Service, "read_decompilation", {{"address", "0x10"}}));
  // The first page did not hold the whole function, which is read after
  // the definition, from its first line, to number its temporaries.
  require(Session.Calls.size() == 3 &&
              Session.Calls[1].Payload ==
                  Json{{"address", "0x10"}, {"offset", 2}} &&
              Session.Calls[1].Revision == "2",
          "the definition is read at its revision");
  require(Session.Calls[2].Payload == Json{{"address", "0x10"},
                                           {"offset", 0},
                                           {"limit", MaxDecompilationLines}} &&
              Session.Calls[2].Revision == "2",
          "the whole function is read at the page's revision");
  require(Header["prelude_lines"] == 2, "the header counts the prelude");
  require(Code == "int f(void) {\n    return 0;\n}\n", "the definition");
  // With prelude=true, the page is the one the worker gives.
  Session.Calls.clear();
  Session.Replies = {First};
  const auto Whole = code(call(Service, "read_decompilation",
                               {{"address", "0x10"}, {"prelude", true}}))
                         .second;
  require(Session.Calls.size() == 2 && Whole.rfind("#include", 0) == 0,
          "the prelude on request");
}

MCP_TEST(LlvmCReadsInTheShortSpellingToo) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Replies = {
      {{"status", "ok"},
       {"revision", "4"},
       {"payload",
        {{"address", "0x10"},
         {"representation", "llvmc"},
         {"text", "uint64_t f(uint32_t a) {\n    return a;\n}\n"},
         {"offset", 0},
         {"total_lines", 3},
         {"next_offset", nullptr},
         {"complete", true}}}}};
  const auto [Header, Code] =
      code(call(Service, "read_decompilation",
                {{"address", "0x10"}, {"representation", "llvmc"}}));
  require(Code == "u64 f(u32 a) {\n    return a;\n}\n", "LLVM-C: " + Code);
  require(Header["legend"].get<std::string>().find("iN/uN") !=
              std::string::npos,
          "the legend lists the types");
}

MCP_TEST(TemporariesReadAlikeOnEveryPage) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  const auto Page = [](const char *Text, int Offset, Json Next) {
    return Json{{"status", "ok"},
                {"revision", "3"},
                {"payload",
                 {{"address", "0x10"},
                  {"dialect", "c"},
                  {"text", Text},
                  {"offset", Offset},
                  {"total_lines", 4},
                  {"next_offset", Next},
                  {"complete", Next.is_null()}}}};
  };
  const char *Head = "int32_t f(void) {\n    int32_t t7_2;\n";
  const char *Tail = "    t7_2 = 1;\n    return t7_2;\n}\n";
  // The second page, then the whole function from its first line.
  Session.Replies = {Page(Tail, 2, nullptr), Page(Head, 0, 2),
                     Page(Tail, 2, nullptr)};
  const auto [Header, Code] = code(call(Service, "read_decompilation",
                                        {{"address", "0x10"}, {"offset", 2}}));
  require(Code == "    t1 = 1;\n    return t1;\n}\n",
          "the page numbers the temporary as the function does: " + Code);
  require(Header["legend"].get<std::string>().find("tN") != std::string::npos,
          "the legend says so");
  require(Session.Calls.size() == 3 && Session.Calls[1].Revision == "3" &&
              Session.Calls[2].Payload["offset"] == 2,
          "the function is read page by page at the page's revision");
}

MCP_TEST(SnapshotCacheEvictionIsExplicit) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {{"status", "ok"},
                      {"revision", "42"},
                      {"payload", {{"text", std::string(MaxResult, 'x')}}}};
  std::string First;
  for (int I = 0; I < 5; ++I) {
    const auto Result = call(Service, "project_metadata");
    if (First.empty())
      First = Result["structuredContent"]["resource_uri"].get<std::string>();
  }
  require(Service.snapshotCount() == MaxSnapshots, "four snapshots are kept");
  require(Service.snapshotBytes() <= MaxCachedResults, "within the budget");
  require(rpcError(Service, request("resources/read", {{"uri", First}})) ==
              -32002,
          "the oldest is gone");
}

MCP_TEST(StandardStdioNotWorkerFraming) {
  FakeBackend Session;
  Server Service(Session);
  const auto Lines =
      served(Service, Session,
             line(initialize()) +
                 line(request("notifications/initialized", nullptr, nullptr)));
  require(Lines.size() == 1, "a notification has no response");
  require(Lines[0]["id"] == 1 && Lines[0]["result"]["protocolVersion"] ==
                                     std::string(ProtocolVersion),
          "initialize is answered");
}

MCP_TEST(MalformedJsonAndBatchReportProtocolErrors) {
  FakeBackend Session;
  Server Service(Session);
  const auto Lines = served(Service, Session,
                            "{bad}\n[]\n{}\n{\"jsonrpc\":\"2.0\",\"id\":true,"
                            "\"method\":\"ping\"}\n{\"a\":1,\"a\":2}\n");
  // The reader refuses what it cannot parse or route at once, while the
  // executor answers the rest, so the two kinds interleave.
  std::vector<int> Codes;
  for (const auto &Each : Lines)
    Codes.push_back(Each["error"]["code"].get<int>());
  std::sort(Codes.begin(), Codes.end());
  require(Codes == std::vector<int>{-32700, -32700, -32600, -32600, -32600},
          "each line is a protocol error");
}

MCP_TEST(AnOverlongMessageEndsTheInput) {
  FakeBackend Session;
  Server Service(Session);
  const auto Lines = served(Service, Session,
                            line(request("ping")) + std::string(MaxLine, ' ') +
                                "\n" + line(request("ping", nullptr, 2)));
  // The reader refuses the long line while the executor answers the ping.
  require(Lines.size() == 2, "the ping and the refusal, nothing after them");
  const bool PingFirst = Lines[0]["id"] == 1;
  require(Lines[PingFirst ? 0 : 1]["id"] == 1 &&
              Lines[PingFirst ? 1 : 0]["error"]["code"] == -32600,
          "the ping before it is answered, and the line refused");
}

MCP_TEST(IdleAnalysisRunsWhileNoRequestWaits) {
  FakeBackend Session;
  Session.IdleWork = 3;
  Server Service(Session);
  // The input stays open until the backend has finished its idle work.
  std::string Output;
  const auto Status = serve(
      Service, Session,
      [&, Calls = 0](char *Into, std::size_t Size) mutable -> std::size_t {
        if (Calls++ == 0) {
          const auto First = line(request("ping"));
          std::copy_n(First.data(), std::min(Size, First.size()), Into);
          return First.size();
        }
        // Wait out the idle steps, then end the input.
        for (int Wait = 0; Wait < 200 && Session.hasIdleWork(); ++Wait)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return 0;
      },
      [&](std::string_view Text) {
        Output += Text;
        return true;
      });
  require(Status == 0 && Session.IdleSteps == 3, "every idle step ran");
  require(transport::parseJson(Output.substr(0, Output.find('\n')))["id"] == 1,
          "the ping is answered");
}

MCP_TEST(ACancelledRequestIsNotAnswered) {
  // The first call holds the executor until the cancellation is read.
  struct Blocking final : Backend {
    std::mutex Lock;
    std::condition_variable Changed;
    bool Released = false;
    unsigned Queries = 0;
    Json query(const std::string &, const Json &,
               const std::optional<std::string> &) override {
      std::unique_lock Guard(Lock);
      ++Queries;
      Changed.wait(Guard, [&] { return Released; });
      return {{"status", "ok"}, {"revision", "1"}, {"payload", Json::object()}};
    }
    bool attached() const override { return false; }
  } Session;
  Server Service(Session);
  const auto Call = [](int ID) {
    return line(request(
        "tools/call",
        {{"name", "project_metadata"}, {"arguments", Json::object()}}, ID));
  };
  std::deque<std::string> Input{
      line(initialize()) +
          line(request("notifications/initialized", nullptr, nullptr)) +
          Call(2) + Call(3) + Call(3),
      line(request("notifications/cancelled", {{"requestId", 3}}, nullptr))};
  std::string Output;
  const auto Status = serve(
      Service, Session,
      [&](char *Into, std::size_t Size) -> std::size_t {
        if (Input.size() == 1) {
          // Hold the cancellation until the first call is running.
          for (int Wait = 0; Wait < 500; ++Wait) {
            {
              std::lock_guard Guard(Session.Lock);
              if (Session.Queries)
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        if (Input.empty()) {
          {
            std::lock_guard Guard(Session.Lock);
            Session.Released = true;
          }
          Session.Changed.notify_all();
          return 0;
        }
        auto Next = std::move(Input.front());
        Input.pop_front();
        require(Next.size() <= Size, "the test chunk fits");
        std::copy(Next.begin(), Next.end(), Into);
        return Next.size();
      },
      [&](std::string_view Text) {
        Output += Text;
        return true;
      });
  require(Status == 0, "serve ends with its input");
  std::vector<Json> Lines;
  for (std::size_t Start = 0; Start < Output.size();) {
    const auto End = Output.find('\n', Start);
    Lines.push_back(transport::parseJson(Output.substr(Start, End - Start)));
    Start = End + 1;
  }
  // initialize, the duplicate id 3 and the call 2; the cancelled 3 has no
  // response.  The reader refuses the duplicate while initialize runs.
  const auto Answer = [&](int ID) {
    for (const auto &Each : Lines)
      if (Each["id"] == ID)
        return Each;
    throw std::runtime_error("no response to " + std::to_string(ID));
  };
  require(Lines.size() == 3, "three responses: " + Output);
  require(Answer(3)["error"]["code"] == -32000,
          "a duplicate id waiting is refused");
  require(Answer(2)["result"]["isError"] == false,
          "the running call is answered");
  require(Session.Queries == 1, "the cancelled call never ran");
}

MCP_TEST(SearchesNameTheCodeTheyFind) {
  FakeBackend Session;
  Server Service(Session);
  ready(Service);
  Session.Response = {
      {"status", "ok"},
      {"revision", "4"},
      {"payload",
       {{"complete", true},
        {"next_address", nullptr},
        {"items", Json::array({{{"address", "0x7004"},
                                {"context", "proxy read too long"},
                                {"segment", ".fini"},
                                {"offset", 4}}})}}}};
  auto Result =
      call(Service, "search", {{"kind", "text"}, {"pattern", "proxy"}});
  require(Session.Calls.back().Payload == Json{{"kind", "text"},
                                               {"pattern", "proxy"},
                                               {"limit", DefaultSearchLimit}},
          "a search is bounded by default");
  require(text(Result) ==
              R"({"revision":"4","complete":true,"next_address":null,)"
              R"("columns":["address","context"],)"
              R"("rows":[["0x7004","proxy read too long"]]})",
          "a text match shows its text: " + text(Result));
  // An immediate search takes its steps and the page after a step's end,
  // then names each match's function and instruction.
  Session.Calls.clear();
  Session.Replies = {
      {{"status", "ok"}, {"revision", "4"}, {"payload", {{"building", true}}}},
      {{"status", "ok"},
       {"revision", "4"},
       {"payload",
        {{"building", false},
         {"complete", false},
         {"next_address", "0x2bd7"},
         {"coverage", "partial"},
         {"items", Json::array({{{"address", "0x2bd7"},
                                 {"context", "Immediate 0x40000000"}}})}}}},
      {{"status", "ok"},
       {"revision", "4"},
       {"payload",
        {{"complete", true},
         {"next_address", nullptr},
         {"items", Json::array()}}}},
      {{"status", "ok"},
       {"revision", "4"},
       {"payload", {{"function_address", "0x2a90"}, {"name", "main"}}}},
      {{"status", "ok"},
       {"revision", "4"},
       {"payload",
        {{"items", Json::array({{{"mnemonic", "mov"},
                                 {"operands", "edx, 0x40000000"}}})}}}}};
  Result =
      call(Service, "search",
           {{"kind", "immediate"}, {"pattern", "0x40000000"}, {"limit", 5}});
  require(Session.Calls.size() == 5 && Session.Calls[1].Operation == "search" &&
              Session.Calls[2].Payload["from"] == "0x2bd7" &&
              Session.Calls[2].Payload["limit"] == 4 &&
              Session.Calls[3].Operation == "resolve" &&
              Session.Calls[4].Operation == "disasm" &&
              Session.Calls[4].Revision == "4",
          "the steps and the page after them, then the function and "
          "instruction at that revision");
  require(text(Result) ==
              R"({"revision":"4","complete":true,"next_address":null,)"
              R"("coverage":"partial",)"
              R"("columns":["address","function","instruction"],)"
              R"("rows":[["0x2bd7","main","mov edx, 0x40000000"]]})",
          "an immediate shows its instruction: " + text(Result));
  const Json Unbounded{{"kind", "bytes"}, {"pattern", "90"}, {"limit", 513}};
  require(rpcError(Service,
                   request("tools/call", {{"name", "search"},
                                          {"arguments", Unbounded}})) == -32602,
          "a search page is bounded");
}
