//===- NeverDCmdStrings.cpp - strings subcommand --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd strings`: the strings of the image in the chosen encodings and
/// code page, or with --refs the instructions that refer to them, as the
/// GUI's Strings and String references windows list them.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../NeverDCLIFunctions.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

using namespace llvm;

namespace neverd::cli {
namespace {

/// Functions one neverd_string_refs_json call decodes.
constexpr int ReferencePageFunctions = 1024;

/// Takes an owned engine string.
std::string take(const char *Text) {
  std::string Value = Text ? Text : "";
  neverd_free_string(Text);
  return Value;
}

/// The engine options the command searches with: the encodings it names,
/// or the engine's defaults, the code page it prefers and the minimum
/// length.
std::string scanOptions() {
  json::Array Names;
  if (StringEncodings.empty()) {
    if (auto Parsed = json::parse(take(neverd_string_encodings_json())))
      if (const auto *Rows = Parsed->getAsArray())
        for (const auto &Row : *Rows)
          if (const auto *Object = Row.getAsObject();
              Object && Object->getBoolean("default").value_or(false))
            if (const auto Name = Object->getString("name"))
              Names.push_back(Name->str());
  } else {
    for (const auto &Name : StringEncodings)
      Names.push_back(Name);
  }
  json::Object Options{{"encodings", std::move(Names)},
                       {"min_length", static_cast<int64_t>(MinStrLen)}};
  if (!StringCodePage.empty())
    Options["preferred"] = StringCodePage.getValue();
  std::string Text;
  raw_string_ostream(Text) << json::Value(std::move(Options));
  return Text;
}

/// The type column: C for plain ASCII, else the encoding's listing
/// spelling.
std::map<std::string, std::string> encodingTypes() {
  std::map<std::string, std::string> Types;
  if (auto Parsed = json::parse(take(neverd_string_encodings_json())))
    if (const auto *Rows = Parsed->getAsArray())
      for (const auto &Row : *Rows)
        if (const auto *Object = Row.getAsObject()) {
          const auto Spelling = Object->getString("spelling").value_or("");
          Types[Object->getString("name").value_or("").str()] =
              Spelling.empty() ? "C" : Spelling.str();
        }
  return Types;
}

/// \p Text with control characters escaped, for one line of a terminal.
std::string printable(StringRef Text) {
  std::string Out;
  for (const unsigned char C : Text) {
    if (C == '\n')
      Out += "\\n";
    else if (C == '\t')
      Out += "\\t";
    else if (C == '\r')
      Out += "\\r";
    else if (C < 0x20 || C == 0x7f)
      Out += "\\x" + utohexstr(C, /*LowerCase=*/false, 2);
    else
      Out += static_cast<char>(C);
  }
  return Out;
}

std::optional<json::Value> parseOrReport(const std::string &Text) {
  auto Parsed = json::parse(Text);
  if (!Parsed) {
    WithColor::error() << "malformed engine reply: "
                       << toString(Parsed.takeError()) << "\n";
    return std::nullopt;
  }
  return std::move(*Parsed);
}

int runStringReferences(neverd_session_t Sess, const std::string &Options) {
  // The strings the references read, by address.
  const char *StringsJson = neverd_strings_ex_json(Sess, Options.c_str());
  if (!StringsJson) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  const auto Strings = parseOrReport(take(StringsJson));
  if (!Strings || !Strings->getAsArray())
    return 1;
  std::map<uint64_t, const json::Object *> ByAddress;
  for (const auto &Row : *Strings->getAsArray())
    if (const auto *Object = Row.getAsObject()) {
      uint64_t Address = 0;
      if (!Object->getString("addr").value_or("").getAsInteger(0, Address))
        ByAddress[Address] = Object;
    }
  const auto Types = encodingTypes();
  const FunctionLocator Functions(Sess);
  json::Array Out;
  size_t Count = 0;
  for (std::optional<neverd_va_t> Cursor = 0; Cursor;) {
    const char *Page = neverd_string_refs_json(Sess, Options.c_str(), *Cursor,
                                               ReferencePageFunctions);
    if (!Page) {
      WithColor::error() << takeLastError(Sess) << "\n";
      return 1;
    }
    const auto Parsed = parseOrReport(take(Page));
    if (!Parsed || !Parsed->getAsObject())
      return 1;
    const auto &Object = *Parsed->getAsObject();
    Cursor.reset();
    if (const auto Next = Object.getString("next_entry")) {
      uint64_t Entry = 0;
      if (!Next->getAsInteger(0, Entry))
        Cursor = Entry;
    }
    const json::Array *Refs = Object.getArray("refs");
    for (const auto &Ref : Refs ? *Refs : json::Array()) {
      // [from, to, string, text_offset, kind, via|null, instruction]
      const json::Array *Row = Ref.getAsArray();
      uint64_t From = 0, String = 0;
      if (!Row || Row->size() != 7 ||
          (*Row)[0].getAsString().value_or("").getAsInteger(0, From) ||
          (*Row)[2].getAsString().value_or("").getAsInteger(0, String) ||
          !ByAddress.count(String)) {
        WithColor::error() << "string references name a string the strings "
                              "list lacks\n";
        return 1;
      }
      const json::Object &Found = *ByAddress[String];
      const std::string Value = Found.getString("value").value_or("").str();
      const auto Offset = (*Row)[3].getAsInteger().value_or(0);
      const std::string Text =
          Offset >= 0 && static_cast<size_t>(Offset) <= Value.size()
              ? Value.substr(static_cast<size_t>(Offset))
              : std::string();
      const std::string Location = Functions.locate(From);
      const std::string Address = "0x" + utohexstr(From);
      if (!StringFilter.empty() &&
          !StringRef(Text).contains_insensitive(StringFilter) &&
          !StringRef(Location).contains_insensitive(StringFilter) &&
          !StringRef(Address).contains_insensitive(StringFilter))
        continue;
      const std::string Encoding =
          Found.getString("encoding").value_or("").str();
      const auto Type = Types.find(Encoding);
      const std::string Instruction =
          (*Row)[6].getAsString().value_or("").str();
      const auto Via = (*Row)[5].getAsString();
      ++Count;
      if (JsonOutput) {
        json::Object Item{
            {"address", Address},
            {"string_address", (*Row)[1]},
            {"text", Text},
            {"encoding", Encoding},
            {"type", Type == Types.end() ? Encoding : Type->second},
            {"kind", (*Row)[4]},
            {"function", Location},
            {"disasm", Instruction}};
        if (Via)
          Item["via"] = Via->str();
        Out.push_back(std::move(Item));
        continue;
      }
      outs() << format("%-18s %-28s %-40s ", Address.c_str(), Location.c_str(),
                       Instruction.c_str())
             << '"' << printable(Text) << '"';
      if (Via)
        outs() << "  via " << *Via;
      outs() << "\n";
    }
  }
  if (JsonOutput)
    outs() << json::Value(std::move(Out)) << "\n";
  else
    outs() << "\n" << Count << " string references found\n";
  return 0;
}

} // namespace

int runStrings(neverd_session_t Sess) {
  const std::string Options = scanOptions();
  if (StringRefs)
    return runStringReferences(Sess, Options);
  const char *Json = neverd_strings_ex_json(Sess, Options.c_str());
  if (!Json) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  const auto Parsed = parseOrReport(take(Json));
  if (!Parsed || !Parsed->getAsArray())
    return 1;
  const auto Types = encodingTypes();
  json::Array Kept;
  for (const auto &Row : *Parsed->getAsArray()) {
    const json::Object *Object = Row.getAsObject();
    if (!Object)
      continue;
    const auto Value = Object->getString("value").value_or("");
    const auto Address = Object->getString("addr").value_or("");
    if (!StringFilter.empty() && !Value.contains_insensitive(StringFilter) &&
        !Address.contains_insensitive(StringFilter))
      continue;
    Kept.push_back(Row);
  }
  if (JsonOutput) {
    outs() << json::Value(std::move(Kept)) << "\n";
    return 0;
  }
  // The type column fits the longest spelling, as windows-1252.
  const auto typeOf = [&](const json::Object &Object) {
    const auto Encoding = Object.getString("encoding").value_or("").str();
    const auto Type = Types.find(Encoding);
    return Type == Types.end() ? Encoding : Type->second;
  };
  size_t TypeWidth = 0;
  for (const auto &Row : Kept)
    TypeWidth = std::max(TypeWidth, typeOf(*Row.getAsObject()).size());
  for (const auto &Row : Kept) {
    const json::Object &Object = *Row.getAsObject();
    const auto Type = typeOf(Object);
    outs() << format("%-18s ",
                     Object.getString("addr").value_or("").str().c_str())
           << Type << std::string(TypeWidth - Type.size() + 1, ' ')
           << printable(Object.getString("value").value_or("")) << "\n";
  }
  outs() << "\n" << Kept.size() << " strings found\n";
  return 0;
}

} // namespace neverd::cli
