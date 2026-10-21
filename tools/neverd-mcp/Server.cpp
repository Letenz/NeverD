//===- Server.cpp - NeverD's read-only MCP server -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JSON-RPC methods, tool calls, resources and result snapshots.
///
//===----------------------------------------------------------------------===//

#include "Server.h"

#include "ResourceLimits.h"

#include <algorithm>
#include <charconv>

namespace neverd::binmcp {
namespace {
bool succeeded(const Json &Response) {
  const auto &Status = member(Response, "status");
  return Status == "ok" || Status == "success";
}

std::string encode(const Json &Value) {
  return Value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json textContent(std::string Text) {
  return {{"type", "text"}, {"text", std::move(Text)}};
}

const Tool *named(const std::vector<const Tool *> &Tools, const Json &Name) {
  if (!Name.is_string())
    return nullptr;
  for (const auto *Candidate : Tools)
    if (Candidate->name() == Name.get_ref<const std::string &>())
      return Candidate;
  return nullptr;
}

const Tool *running(const std::vector<Tool> &Tools,
                    std::string_view Operation) {
  for (const auto &Candidate : Tools)
    if (Candidate.Operation == Operation)
      return &Candidate;
  return nullptr;
}

bool validUTF8(std::string_view Text) {
  for (std::size_t I = 0; I < Text.size();) {
    const auto Lead = static_cast<unsigned char>(Text[I]);
    const std::size_t Length = Lead < 0x80           ? 1
                               : (Lead >> 5) == 0x6  ? 2
                               : (Lead >> 4) == 0xE  ? 3
                               : (Lead >> 3) == 0x1E ? 4
                                                     : 0;
    if (!Length || I + Length > Text.size())
      return false;
    for (std::size_t Next = 1; Next < Length; ++Next)
      if ((static_cast<unsigned char>(Text[I + Next]) & 0xC0) != 0x80)
        return false;
    I += Length;
  }
  return true;
}

/// A query component with its percent escapes, and + for a space, decoded.
std::optional<std::string> decoded(std::string_view Text) {
  std::string Result;
  for (std::size_t I = 0; I < Text.size(); ++I) {
    const auto Hex = [](char C) {
      return (C >= '0' && C <= '9')   ? C - '0'
             : (C >= 'a' && C <= 'f') ? C - 'a' + 10
             : (C >= 'A' && C <= 'F') ? C - 'A' + 10
                                      : -1;
    };
    if (Text[I] == '+')
      Result += ' ';
    else if (Text[I] == '%' && I + 2 < Text.size() && Hex(Text[I + 1]) >= 0 &&
             Hex(Text[I + 2]) >= 0) {
      Result += static_cast<char>(Hex(Text[I + 1]) * 16 + Hex(Text[I + 2]));
      I += 2;
    } else
      Result += Text[I];
  }
  if (!validUTF8(Result))
    return std::nullopt;
  return Result;
}

/// The name=value fields of a URI query, in order.
std::vector<std::pair<std::string, std::string>>
parameters(std::string_view Query) {
  std::vector<std::pair<std::string, std::string>> Fields;
  while (!Query.empty()) {
    const auto End = Query.find('&');
    const auto Field = Query.substr(0, End);
    Query = End == std::string_view::npos ? std::string_view()
                                          : Query.substr(End + 1);
    if (Field.empty())
      continue;
    const auto Equals = Field.find('=');
    auto Name = decoded(Field.substr(0, Equals));
    auto Value =
        decoded(Equals == std::string_view::npos ? std::string_view()
                                                 : Field.substr(Equals + 1));
    if (!Name || !Value)
      throw RpcError{-32602, "Invalid resource parameter"};
    Fields.emplace_back(std::move(*Name), std::move(*Value));
  }
  return Fields;
}

std::optional<std::int64_t> number(std::string_view Text) {
  std::int64_t Value = 0;
  const auto [End, Error] =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value);
  if (Error != std::errc() || End != Text.data() + Text.size() || Text.empty())
    return std::nullopt;
  return Value;
}

Json resourceContents(const std::string &URI, std::string Text) {
  return {{"contents", Json::array({{{"uri", URI},
                                     {"mimeType", "application/json"},
                                     {"text", std::move(Text)}}})}};
}
} // namespace

std::size_t Server::Snapshot::byteAt(std::size_t Character) const {
  if (Character >= Characters)
    return Text.size();
  std::size_t At = Marks[Character / Stride];
  for (auto Left = Character % Stride; Left; --Left)
    do
      ++At;
    while (At < Text.size() &&
           (static_cast<unsigned char>(Text[At]) & 0xC0) == 0x80);
  return At;
}

Server::Server(Backend &Session) : Session(Session) {
  for (const auto &Listed : projectTools())
    Tools.push_back(&Listed);
  if (Session.attached())
    for (const auto &Listed : workbenchTools())
      Tools.push_back(&Listed);
}

Json Server::handle(const Json &Request) {
  if (!Request.is_object() || member(Request, "jsonrpc") != "2.0" ||
      !member(Request, "method").is_string())
    throw RpcError{-32600, "Invalid JSON-RPC request"};
  if (const auto &ID = member(Request, "id");
      Request.contains("id") && !ID.is_string() && !ID.is_number_integer())
    throw RpcError{-32600, "Invalid request id"};
  const auto &Method = Request["method"].get_ref<const std::string &>();
  const Json Params =
      Request.contains("params") ? Request["params"] : Json::object();
  if (!Params.is_object())
    throw RpcError{-32602, "params must be an object"};
  if (Method == "initialize") {
    if (Initialized)
      throw RpcError{-32600, "Session already initialized"};
    if (!member(Params, "protocolVersion").is_string() ||
        !member(Params, "capabilities").is_object() ||
        !member(Params, "clientInfo").is_object())
      throw RpcError{-32602, "Missing initialization parameters"};
    Initialized = true;
    // A client of the previous revision gets it: every result is the same.
    const auto &Requested = Params["protocolVersion"];
    return {{"protocolVersion", Requested == "2025-06-18"
                                    ? Requested
                                    : Json(std::string(ProtocolVersion))},
            {"capabilities",
             {{"tools", Json::object()}, {"resources", Json::object()}}},
            {"serverInfo", {{"name", "neverd-mcp"}, {"version", "0.2.0"}}}};
  }
  if (Method == "ping")
    return Json::object();
  if (Method == "notifications/initialized") {
    if (Initialized)
      Ready = true;
    return nullptr;
  }
  if (!Ready)
    throw RpcError{-32002, "Initialize the MCP session first"};
  if (Method == "tools/list") {
    if (truthy(member(Params, "cursor")))
      throw RpcError{-32602, "Unknown tool cursor"};
    Json Listed = Json::array();
    for (const auto *Each : Tools)
      Listed.push_back(Each->Listed);
    return {{"tools", std::move(Listed)}};
  }
  if (Method == "tools/call") {
    const auto *Selected = named(Tools, member(Params, "name"));
    if (!Selected)
      throw RpcError{-32602, "Unknown tool"};
    const Json Arguments =
        Params.contains("arguments") ? Params["arguments"] : Json::object();
    validate(Arguments, Selected->schema());
    return callTool(*Selected, Arguments);
  }
  if (Method == "resources/list") {
    if (truthy(member(Params, "cursor")))
      throw RpcError{-32602, "Unknown resource cursor"};
    Json Resources = Json::array({{{"uri", "neverd://project/metadata"},
                                   {"name", "Project metadata"},
                                   {"mimeType", "application/json"}},
                                  {{"uri", "neverd://project/functions"},
                                   {"name", "Functions (first page)"},
                                   {"mimeType", "application/json"}}});
    if (Session.attached())
      Resources.push_back({{"uri", "neverd://gui/selection"},
                           {"name", "GUI selection"},
                           {"mimeType", "application/json"}});
    for (const auto &Kept : Snapshots)
      Resources.push_back(
          {{"uri", Kept.URI},
           {"name",
            "Result snapshot " + Kept.URI.substr(Kept.URI.rfind('/') + 1)},
           {"mimeType", Kept.MimeType}});
    return {{"resources", std::move(Resources)}};
  }
  if (Method == "resources/templates/list") {
    const auto Template = [](const char *URI, const char *Name) {
      return Json{{"uriTemplate", URI},
                  {"name", Name},
                  {"mimeType", "application/json"}};
    };
    return {{"resourceTemplates",
             Json::array(
                 {Template("neverd://project/functions{?offset,limit,filter,"
                           "expected_revision}",
                           "Function page"),
                  Template("neverd://project/"
                           "disasm{?address,limit,expected_revision}",
                           "Disassembly"),
                  Template("neverd://project/decompile{?address,"
                           "representation,offset,limit,expected_revision}",
                           "C and IR"),
                  Template("neverd://project/xrefs{?address,direction,offset,"
                           "limit,expected_revision}",
                           "Cross references")})}};
  }
  if (Method == "resources/read")
    return readResource(member(Params, "uri"));
  if (Method.rfind("notifications/", 0) == 0)
    return nullptr;
  throw RpcError{-32601, "Method not found"};
}

std::optional<Json>
Server::resolveAddress(const std::string &Operation, Json &Arguments,
                       const std::optional<std::string> &Revision) {
  const auto It = Arguments.find("address");
  if (It == Arguments.end() || !It->is_string() ||
      hexAddress(It->get_ref<const std::string &>()))
    return std::nullopt;
  // A name, resolved as the workbench resolves what it shows; code views
  // take the entry of the function it lies in.
  auto Resolved = Session.query("resolve", {{"query", *It}}, Revision);
  if (!succeeded(Resolved))
    return Resolved;
  const auto &Found = member(Resolved, "payload");
  const bool CodeView = Operation == "decompile" || Operation == "cfg";
  const auto &Entry = member(Found, "function_address");
  Arguments["address"] =
      CodeView && truthy(Entry) ? Entry : member(Found, "address");
  return std::nullopt;
}

Json Server::callTool(const Tool &Selected, Json Arguments) {
  std::optional<std::string> Revision;
  if (const auto It = Arguments.find("expected_revision");
      It != Arguments.end()) {
    Revision = It->get<std::string>();
    Arguments.erase(It);
  }
  const auto &Operation = Selected.Operation;
  if (Operation == "strings" && !Arguments.contains("text_limit"))
    Arguments["text_limit"] = DefaultStringTextLimit;
  else if (Operation == "call_paths") {
    Json Waypoints = Json::array({Arguments["from"]});
    for (const auto &Via : member(Arguments, "via"))
      Waypoints.push_back(Via);
    Waypoints.push_back(Arguments["to"]);
    for (const char *Key : {"from", "via", "to"})
      Arguments.erase(Key);
    Arguments["waypoints"] = std::move(Waypoints);
  } else if (Operation == "call_tree") {
    Arguments["root"] = Arguments["function"];
    Arguments["direction"] =
        truthy(member(Arguments, "callers")) ? "callers" : "callees";
    Arguments.erase("function");
    Arguments.erase("callers");
  } else if (Operation == "search" && !Arguments.contains("limit"))
    Arguments["limit"] = DefaultSearchLimit;
  try {
    if (auto Refused = resolveAddress(Operation, Arguments, Revision))
      return textResult(
          compactResult("resolve", Json::object(), *Refused).first, true,
          *Refused);
    const bool Prelude = truthy(member(Arguments, "prelude"));
    const bool Exact = truthy(member(Arguments, "exact"));
    Arguments.erase("prelude");
    Arguments.erase("exact");
    auto Response = Operation == "search"
                        ? search(Arguments)
                        : Session.query(Operation, Arguments, Revision);
    // A page from the first line to the last is the whole function.
    std::optional<std::string> Function;
    if (Operation == "decompile" && !Exact)
      Function = pagedFunction(Response);
    const Json Lines =
        member(member(member(Response, "payload"), "prelude"), "lines");
    if (Operation == "decompile" && !Prelude && truthy(Lines) &&
        !truthy(member(Arguments, "offset"))) {
      // A source page starts at the definition: the lines before it make
      // the file compile and are read on request.
      Json Definition = Arguments;
      Definition["offset"] = Lines;
      const auto &Taken = member(Response, "revision");
      Response = Session.query(Operation, Definition,
                               Taken.is_string()
                                   ? std::optional(Taken.get<std::string>())
                                   : std::nullopt);
      if (member(Response, "payload").is_object())
        Response["payload"]["prelude_lines"] = Lines;
    }
    if (Operation == "decompile" && !Exact && !Function)
      Function = wholeFunction(Arguments, Response);
    if (Function && member(Response, "payload").is_object())
      Response["payload"]["function_text"] = std::move(*Function);
    Arguments["exact"] = Exact;
    auto [Text, IsError] = compactResult(Operation, Arguments, Response);
    return textResult(std::move(Text), IsError, Response);
  } catch (const std::exception &Failure) {
    return {{"content", Json::array({textContent(Failure.what())})},
            {"isError", true}};
  }
}

namespace {
/// Whether \p Payload is a page of C.
bool cPage(const Json &Payload) {
  const auto &Dialect = member(Payload, "dialect");
  return (truthy(Dialect) ? Dialect : member(Payload, "representation")) == "c";
}
} // namespace

std::optional<std::string> Server::pagedFunction(const Json &Response) {
  const auto &Payload = member(Response, "payload");
  const auto &Text = member(Payload, "text");
  if (!succeeded(Response) || !cPage(Payload) || !Text.is_string() ||
      member(Payload, "offset") != 0 || !truthy(member(Payload, "complete")))
    return std::nullopt;
  return Text.get<std::string>();
}

std::optional<std::string> Server::wholeFunction(const Json &Arguments,
                                                 const Json &Response) {
  if (!succeeded(Response) || !cPage(member(Response, "payload")))
    return std::nullopt;
  // The pages are the function's lines in order; the engine keeps the text
  // it paged.  The page read stands without the whole when that fails.
  const auto &Taken = member(Response, "revision");
  const auto Revision = Taken.is_string()
                            ? std::optional(Taken.get<std::string>())
                            : std::nullopt;
  Json Request = Arguments;
  Request["offset"] = 0;
  Request["limit"] = MaxDecompilationLines;
  std::string Whole;
  try {
    for (unsigned Page = 0; Page < MaxDecompilationPages; ++Page) {
      const auto Reply = Session.query("decompile", Request, Revision);
      const auto &Part = member(Reply, "payload");
      const auto &Lines = member(Part, "text");
      if (!succeeded(Reply) || !Lines.is_string())
        return std::nullopt;
      Whole += Lines.get_ref<const std::string &>();
      if (truthy(member(Part, "complete")))
        return Whole;
      const auto &Next = member(Part, "next_offset");
      if (!Next.is_number_integer() || Next.get<std::int64_t>() <= 0 ||
          Next == member(Request, "offset"))
        return std::nullopt;
      Request["offset"] = Next;
    }
  } catch (const std::exception &) {
  }
  return std::nullopt;
}

Json Server::search(const Json &Arguments) {
  // An immediate search decodes the image in bounded steps: the same
  // request takes the next step, and a request from next_address the page
  // after a step's end.  The page is filled here, not by more calls.
  const auto Page = [&](const Json &Request) {
    auto Reply = Session.query("search", Request, std::nullopt);
    for (unsigned Step = 0;
         Step < ::neverd::limits::mcp::kServerPageStepLimit &&
         succeeded(Reply) &&
         truthy(member(member(Reply, "payload"), "building"));
         ++Step)
      Reply = Session.query("search", Request, std::nullopt);
    return Reply;
  };
  auto Response = Page(Arguments);
  const auto Limit = Arguments.at("limit").get<std::size_t>();
  for (unsigned Step = 0;
       Step < ::neverd::limits::mcp::kServerServerSearchStepLimit &&
       succeeded(Response);
       ++Step) {
    auto &Found = Response["payload"];
    const auto &Next = member(Found, "next_address");
    if (truthy(member(Found, "complete")) || !Next.is_string() ||
        !member(Found, "items").is_array() || Found["items"].size() >= Limit)
      break;
    Json Request = Arguments;
    Request["from"] = Next;
    Request["limit"] = Limit - Found["items"].size();
    auto More = Page(Request);
    const auto &Rest = member(More, "payload");
    if (!succeeded(More) || More["revision"] != Response["revision"] ||
        !member(Rest, "items").is_array())
      break;
    for (const auto &Item : Rest["items"])
      Found["items"].push_back(Item);
    Found["complete"] = member(Rest, "complete");
    Found["next_address"] = member(Rest, "next_address");
    if (member(Rest, "coverage") == "partial")
      Found["coverage"] = "partial";
  }
  if (!succeeded(Response) || member(Arguments, "kind") == "text" ||
      !member(member(Response, "payload"), "items").is_array())
    return Response;
  // A code match names its function, and an immediate its instruction, at
  // the revision the matches were found at.
  const auto &Taken = member(Response, "revision");
  const auto Revision = Taken.is_string()
                            ? std::optional(Taken.get<std::string>())
                            : std::nullopt;
  const bool Immediate = member(Arguments, "kind") == "immediate";
  for (auto &Item : Response["payload"]["items"]) {
    const auto &Address = member(Item, "address");
    if (!Address.is_string())
      continue;
    const auto Found = Session.query("resolve", {{"query", Address}}, Revision);
    if (const auto &At = member(Found, "payload");
        succeeded(Found) && truthy(member(At, "function_address")))
      Item["function"] = member(At, "name");
    if (!Immediate)
      continue;
    const auto Code =
        Session.query("disasm", {{"address", Address}, {"limit", 1}}, Revision);
    const auto &Lines = member(member(Code, "payload"), "items");
    if (!succeeded(Code) || !Lines.is_array() || Lines.empty())
      continue;
    const auto &Mnemonic = member(Lines[0], "mnemonic");
    const auto &Operands = member(Lines[0], "operands");
    if (Mnemonic.is_string() && Operands.is_string())
      Item["instruction"] =
          Mnemonic.get<std::string>() + " " + Operands.get<std::string>();
  }
  return Response;
}

Json Server::textResult(std::string Text, bool IsError, const Json &Response,
                        const char *MimeType, const char *Encoding) {
  const auto Size = Text.size();
  if (MaxResult < Size && Size <= MaxCachedResults) {
    while (!Snapshots.empty() && (Snapshots.size() >= MaxSnapshots ||
                                  CachedBytes + Size > MaxCachedResults)) {
      CachedBytes -= Snapshots.front().Text.size();
      Snapshots.pop_front();
    }
    Snapshot Kept;
    Kept.URI = "neverd://result/" + std::to_string(++SnapshotSerial);
    Kept.MimeType = MimeType;
    Kept.Encoding = Encoding;
    Kept.Revision = member(Response, "revision");
    Kept.ProjectID = member(Response, "project_id");
    for (std::size_t I = 0; I < Size; ++I)
      if ((static_cast<unsigned char>(Text[I]) & 0xC0) != 0x80) {
        if (Kept.Characters % Snapshot::Stride == 0)
          Kept.Marks.push_back(I);
        ++Kept.Characters;
      }
    Kept.Text = std::move(Text);
    const Json Reference{{"status", member(Response, "status")},
                         {"project_id", Kept.ProjectID},
                         {"revision", Kept.Revision},
                         {"resource_uri", Kept.URI},
                         {"total_characters", Kept.Characters},
                         {"inline_complete", false}};
    Json Link{{"type", "resource_link"},
              {"uri", Kept.URI},
              {"name", "NeverD result snapshot"},
              {"mimeType", MimeType},
              {"description", "Read this snapshot in character pages using "
                              "offset and limit URI parameters."}};
    CachedBytes += Size;
    Snapshots.push_back(std::move(Kept));
    return {{"content",
             Json::array({std::move(Link), textContent(encode(Reference))})},
            {"structuredContent", Reference},
            {"isError", IsError}};
  }
  if (Size > MaxCachedResults) {
    Text = encode({{"status", "budget_exceeded"},
                   {"revision", member(Response, "revision")},
                   {"error",
                    {{"message", "Result exceeds MCP 8 MiB snapshot budget; "
                                 "request a smaller page/window."}}}});
    IsError = true;
  }
  // No structuredContent: no tool declares an output schema, and a copy of
  // the text would double what a client may forward to its model.
  return {{"content", Json::array({textContent(std::move(Text))})},
          {"isError", IsError}};
}

Json Server::readResource(const Json &Given) {
  if (!Given.is_string())
    throw RpcError{-32602, "uri must be a string"};
  const auto &URI = Given.get_ref<const std::string &>();
  constexpr std::string_view Scheme = "neverd://";
  if (URI.rfind(Scheme, 0) != 0 || URI.find('#') != std::string::npos)
    throw RpcError{-32602, "Unsupported resource URI"};
  const std::string_view Rest = std::string_view(URI).substr(Scheme.size());
  const auto AuthorityEnd = std::min(Rest.find_first_of("/?"), Rest.size());
  const auto Authority = Rest.substr(0, AuthorityEnd);
  if (Authority.find_first_of("@:") != std::string_view::npos)
    throw RpcError{-32602, "Unsupported resource URI"};
  const auto Remainder = Rest.substr(AuthorityEnd);
  const auto QueryAt = std::min(Remainder.find('?'), Remainder.size());
  const auto Path = Remainder.substr(0, QueryAt);
  const auto Query = Remainder.substr(std::min(QueryAt + 1, Remainder.size()));
  if (Authority == "result")
    return readSnapshot(URI, Path, Query);
  auto Operation = Path;
  while (!Operation.empty() && Operation.front() == '/')
    Operation.remove_prefix(1);
  const Tool *Selected = nullptr;
  if (Authority == "gui" && Operation == "selection" && Session.attached())
    Selected = running(workbenchTools(), Operation);
  else if (Authority == "project")
    Selected = running(projectTools(), Operation);
  if (!Selected)
    throw RpcError{-32002, "Resource not found"};
  Json Arguments = Json::object();
  for (auto &[Key, Value] : parameters(Query)) {
    if (Arguments.contains(Key))
      throw RpcError{-32602, "Repeated resource parameter"};
    if (Key == "offset" || Key == "limit" || Key == "size") {
      const auto Number = number(Value);
      if (!Number)
        throw RpcError{-32602, "Invalid numeric resource parameter"};
      Arguments[Key] = *Number;
    } else
      Arguments[Key] = std::move(Value);
  }
  validate(Arguments, Selected->schema());
  std::optional<std::string> Revision;
  if (const auto It = Arguments.find("expected_revision");
      It != Arguments.end()) {
    Revision = It->get<std::string>();
    Arguments.erase(It);
  }
  // A resource is the worker's complete response.
  auto Response = resolveAddress(Selected->Operation, Arguments, Revision);
  if (!Response)
    Response = Session.query(Selected->Operation, Arguments, Revision);
  auto Text = encode(*Response);
  if (!succeeded(*Response))
    throw RpcError{-32002, std::move(Text)};
  auto Result = textResult(std::move(Text), false, *Response,
                           "application/json", "json-text");
  auto &Content = Result["content"].back();
  return resourceContents(URI, Content["text"].get<std::string>());
}

Json Server::readSnapshot(const std::string &URI, std::string_view Path,
                          std::string_view Query) {
  if (Path.size() < 2 || Path[0] != '/' || Path[1] == '0' ||
      !std::all_of(Path.begin() + 1, Path.end(),
                   [](char C) { return C >= '0' && C <= '9'; }))
    throw RpcError{-32002, "Result snapshot not found"};
  const auto Key = "neverd://result" + std::string(Path);
  const auto Kept =
      std::find_if(Snapshots.begin(), Snapshots.end(),
                   [&](const Snapshot &Each) { return Each.URI == Key; });
  if (Kept == Snapshots.end())
    throw RpcError{
        -32002,
        "Result snapshot expired or not found; repeat the original query"};
  std::optional<std::string> Offset, Limit, Expected;
  for (auto &[Name, Value] : parameters(Query)) {
    auto *Slot = Name == "offset"              ? &Offset
                 : Name == "limit"             ? &Limit
                 : Name == "expected_revision" ? &Expected
                                               : nullptr;
    if (!Slot || *Slot)
      throw RpcError{-32602, "Invalid result page parameters"};
    *Slot = std::move(Value);
  }
  const auto Start = number(Offset.value_or("0"));
  const auto Count = number(Limit.value_or("32768"));
  if (!Start || !Count)
    throw RpcError{-32602, "Invalid result page range"};
  if (*Start < 0 || static_cast<std::uint64_t>(*Start) > Kept->Characters ||
      *Count < 1 ||
      *Count > ::neverd::limits::mcp::kServerReadSnapshotCountLimit)
    throw RpcError{-32602, "Result page is outside its bounds"};
  if (Expected && Kept->Revision != *Expected)
    throw RpcError{-32602, "Result snapshot revision does not match"};
  const auto First = static_cast<std::size_t>(*Start);
  const auto End =
      std::min(First + static_cast<std::size_t>(*Count), Kept->Characters);
  const auto From = Kept->byteAt(First), To = Kept->byteAt(End);
  const Json Page{
      {"project_id", Kept->ProjectID},
      {"revision", Kept->Revision},
      {"encoding", Kept->Encoding},
      {"offset", First},
      {"total_characters", Kept->Characters},
      {"text", Kept->Text.substr(From, To - From)},
      {"next_offset", End < Kept->Characters ? Json(End) : Json(nullptr)},
      {"complete", End == Kept->Characters}};
  return resourceContents(URI, encode(Page));
}

} // namespace neverd::binmcp
