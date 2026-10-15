//===- NeverDCmdTypes.cpp - The local types command -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd types`: the program's local types as the workbench's Local Types
/// window shows them.  Without --type it lists them; --type lays a type out
/// (--layout) or renders it, with what it needs, as C, C++, Rust or Go;
/// --all renders every type as one unit that compiles.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using namespace llvm;

namespace neverd::cli {
namespace {

bool knownLanguage(StringRef Id) {
  return Id == "c" || Id == "cpp" || Id == "rust" || Id == "go";
}

bool knownSpelling(StringRef Id) {
  return Id == "windows" || Id == "ida" || Id == "keywords" || Id == "portable";
}

std::string text(const json::Value &Value) {
  std::string Out;
  raw_string_ostream(Out) << Value;
  return Out;
}

/// The engine's JSON reply, or none with an error printed.
std::optional<json::Value> reply(neverd_session_t Sess, const char *Raw,
                                 StringRef What) {
  if (!Raw) {
    WithColor::error() << What << ": " << takeLastError(Sess) << "\n";
    return std::nullopt;
  }
  auto Parsed = json::parse(Raw);
  neverd_free_string(Raw);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    WithColor::error() << What << ": the engine returned invalid JSON\n";
    return std::nullopt;
  }
  return std::move(*Parsed);
}

std::string hexText(int64_t Value) {
  std::string Out;
  raw_string_ostream(Out) << format_hex(static_cast<uint64_t>(Value), 0,
                                        /*Upper=*/true);
  if (Out.size() > 1)
    Out[1] = 'x';
  return Out;
}

int list(neverd_session_t Sess, raw_ostream &Out) {
  json::Object Options;
  if (!TypesFilter.empty())
    Options["filter"] = TypesFilter;
  if (TypesLanguage.getNumOccurrences())
    Options["language"] = TypesLanguage;
  std::optional<json::Value> List = reply(
      Sess, neverd_local_types_json(Sess, text(std::move(Options)).c_str()),
      "types");
  if (!List)
    return 1;
  if (JsonOutput) {
    Out << *List << "\n";
    return 0;
  }
  const json::Object &Result = *List->getAsObject();
  const json::Array *Types = Result.getArray("types");
  const json::Object *Stats = Result.getObject("stats");
  const StringRef Source = Result.getString("source").value_or("none");
  Out << "\nLocal types (" << (Types ? Types->size() : 0) << ", "
      << (Source == "pdb" ? "from the PDB" : "none read") << ", "
      << Result.getString("dataModel").value_or("") << "):\n";
  if (Stats && Source == "pdb")
    Out << format(
        "  %lld structures, %lld unions, %lld enumerations, %lld "
        "declared only\n",
        static_cast<long long>(Stats->getInteger("structs").value_or(0)),
        static_cast<long long>(Stats->getInteger("unions").value_or(0)),
        static_cast<long long>(Stats->getInteger("enums").value_or(0)),
        static_cast<long long>(Stats->getInteger("declaredOnly").value_or(0)));
  if (Types) {
    Out << format("  %6s  %-8s %8s %7s  %s\n", "#", "Kind", "Size", "Members",
                  "Name");
    for (const json::Value &Value : *Types) {
      const json::Object *Type = Value.getAsObject();
      if (!Type)
        continue;
      const std::optional<int64_t> Size = Type->getInteger("size");
      const std::optional<int64_t> Members = Type->getInteger("members");
      Out << format(
          "  %6lld  %-8s %8s %7s  %s\n",
          static_cast<long long>(Type->getInteger("ordinal").value_or(0)),
          Type->getString("kind").value_or("").str().c_str(),
          Size ? hexText(*Size).c_str() : "-",
          Members ? std::to_string(*Members).c_str() : "-",
          Type->getString("name").value_or("").str().c_str());
    }
  }
  if (Stats)
    if (const json::Array *Mismatches = Stats->getArray("nestingMismatches"))
      for (const json::Value &Name : *Mismatches)
        WithColor::warning()
            << "the members of " << Name.getAsString().value_or("")
            << " do not lay out as the PDB lists them\n";
  if (const json::Array *Renamed = Result.getArray("renamed"))
    for (const json::Value &Pair : *Renamed)
      if (const json::Array *Names = Pair.getAsArray();
          Names && Names->size() == 2)
        WithColor::note() << TypesLanguage << " spells "
                          << (*Names)[0].getAsString().value_or("") << " as "
                          << (*Names)[1].getAsString().value_or("") << "\n";
  return 0;
}

/// \p Members as rows of the layout table, a record in place followed by
/// its own members indented.
void layoutRows(const json::Array &Members, unsigned Depth, raw_ostream &Out) {
  for (const json::Value &Value : Members) {
    const json::Object *Member = Value.getAsObject();
    if (!Member)
      continue;
    const StringRef Kind = Member->getString("kind").value_or("");
    const int64_t Offset = Member->getInteger("offset").value_or(0);
    const int64_t Size = Member->getInteger("size").value_or(0);
    std::string Where = hexText(Offset);
    std::string Width = hexText(Size);
    if (Kind == "bitfield") {
      const int64_t Storage = Member->getInteger("storageOffset").value_or(0);
      const int64_t Bit =
          Member->getInteger("bitOffset").value_or(0) - Storage * 8;
      const int64_t Bits = Member->getInteger("bitWidth").value_or(0);
      Where += "." + std::to_string(Bit);
      Width = ":" + std::to_string(Bits);
    }
    std::string Name = std::string(Depth * 2, ' ');
    std::string Type = Member->getString("type").value_or("").str();
    if (Kind == "padding") {
      Name += "(padding)";
    } else {
      Name += Member->getString("name").value_or("").str();
      if (Kind == "struct" || Kind == "union")
        Type = Kind.str() + (Name.find_first_not_of(' ') == std::string::npos
                                 ? " (anonymous)"
                                 : "");
    }
    Out << format("  %-10s %-6s %-40s %s\n", Where.c_str(), Width.c_str(),
                  Name.c_str(), Type.c_str());
    if (const json::Array *Inner = Member->getArray("members"))
      layoutRows(*Inner, Depth + 1, Out);
  }
}

int layout(neverd_session_t Sess, raw_ostream &Out) {
  const std::string Options = text(json::Object{{"spelling", TypesSpelling}});
  json::Array All;
  for (const std::string &Name : TypeNames) {
    std::optional<json::Value> Type =
        reply(Sess, neverd_local_type_json(Sess, Name.c_str(), Options.c_str()),
              "types");
    if (!Type)
      return 1;
    if (JsonOutput) {
      All.push_back(std::move(*Type));
      continue;
    }
    const json::Object &Object = *Type->getAsObject();
    Out << "\n" << Object.getString("kind").value_or("") << " " << Name;
    if (std::optional<int64_t> Size = Object.getInteger("size"))
      Out << ": " << hexText(*Size) << " (" << *Size << ") bytes";
    if (std::optional<int64_t> Align = Object.getInteger("alignment"))
      Out << ", aligned to " << *Align;
    if (std::optional<int64_t> Pack = Object.getInteger("pack"); Pack && *Pack)
      Out << ", packed to " << *Pack;
    Out << "\n";
    if (const json::Array *Members = Object.getArray("members")) {
      Out << format("  %-10s %-6s %-40s %s\n", "Offset", "Size", "Member",
                    "Type");
      layoutRows(*Members, 0, Out);
    }
    if (const json::Array *Values = Object.getArray("values"))
      for (const json::Value &Value : *Values)
        if (const json::Object *Entry = Value.getAsObject())
          Out << "  " << Entry->getString("name").value_or("") << " = "
              << *Entry->get("value") << "\n";
    if (std::optional<StringRef> Target = Object.getString("target"))
      Out << "  = " << *Target << "\n";
    if (std::optional<StringRef> Declaration = Object.getString("declaration"))
      Out << "  " << *Declaration << "\n";
  }
  if (JsonOutput)
    Out << json::Value(std::move(All)) << "\n";
  return 0;
}

int render(neverd_session_t Sess, raw_ostream &Out) {
  json::Object Options{{"language", TypesLanguage},
                       {"spelling", TypesSpelling},
                       {"offsets", !TypesNoOffsets},
                       {"hexOffsets", !TypesDecimalOffsets},
                       {"asserts", static_cast<bool>(TypesAsserts)}};
  if (!TypesAll) {
    json::Array Names;
    for (const std::string &Name : TypeNames)
      Names.push_back(Name);
    Options["types"] = std::move(Names);
    Options["dependencies"] = !TypesAlone;
  }
  const char *Source =
      neverd_local_types_source(Sess, text(std::move(Options)).c_str());
  if (!Source) {
    WithColor::error() << "types: " << takeLastError(Sess) << "\n";
    return 1;
  }
  if (JsonOutput)
    Out << json::Value(
               json::Object{{"language", TypesLanguage}, {"source", Source}})
        << "\n";
  else
    Out << Source;
  neverd_free_string(Source);
  return 0;
}

} // namespace

int runTypes(neverd_session_t Sess) {
  if (!knownLanguage(TypesLanguage)) {
    WithColor::error() << "--lang must be c, cpp, rust or go\n";
    return 1;
  }
  if (!knownSpelling(TypesSpelling)) {
    WithColor::error()
        << "--spelling must be windows, ida, keywords or portable\n";
    return 1;
  }
  if (TypesAll && !TypeNames.empty()) {
    WithColor::error() << "--all renders every type; leave out --type\n";
    return 1;
  }
  if ((TypesLayout || TypesAlone) && TypeNames.empty()) {
    WithColor::error() << "--layout and --alone need a --type\n";
    return 1;
  }
  std::unique_ptr<raw_fd_ostream> File;
  if (!OutputFile.empty()) {
    std::error_code EC;
    File = std::make_unique<raw_fd_ostream>(OutputFile, EC);
    if (EC) {
      WithColor::error() << "cannot write " << OutputFile << ": "
                         << EC.message() << "\n";
      return 1;
    }
  }
  raw_ostream &Out = File ? *File : outs();
  if (TypeNames.empty() && !TypesAll)
    return list(Sess, Out);
  if (TypesLayout)
    return layout(Sess, Out);
  return render(Sess, Out);
}

} // namespace neverd::cli
