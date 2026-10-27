//===- NeverDCmdTypeFit.cpp - Which local types a pointer fits ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd types --fit`: the structures and unions a pointer may point at,
/// best first, judged by how the accesses through it land on their members,
/// as the workbench's structure offset and type pickers rank them.
/// `neverd types --type T --member offset[:size]`: where an access lands in
/// T, as a structure offset names it.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace neverd::cli {
namespace {

/// A number as an option spells it: 0x hexadecimal or decimal, with a sign.
std::optional<int64_t> number(StringRef Text) {
  Text = Text.trim();
  const bool Negative = Text.consume_front("-");
  uint64_t Value = 0;
  const bool Hex = Text.consume_front("0x") || Text.consume_front("0X");
  if (Text.empty() || Text.getAsInteger(Hex ? 16 : 10, Value) ||
      Value > static_cast<uint64_t>(INT64_MAX))
    return std::nullopt;
  return Negative ? -static_cast<int64_t>(Value) : static_cast<int64_t>(Value);
}

/// An offset as the listing writes it: `+0x10`, `-0x8`.
std::string offsetText(int64_t Offset) {
  return (Offset < 0 ? "-0x" : "+0x") +
         utohexstr(static_cast<uint64_t>(Offset < 0 ? -Offset : Offset));
}

/// The engine's JSON reply, or none with the session's error reported.
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

/// The function --func names: by name, else by its entry address.
std::optional<uint64_t> functionEntry(neverd_session_t Sess) {
  int Index = neverd_func_find_by_name(Sess, TypesFunc.c_str());
  if (Index < 0)
    if (std::optional<uint64_t> Addr = parseAddrArg(TypesFunc))
      Index = neverd_func_find_by_addr(Sess, *Addr);
  if (Index < 0)
    return std::nullopt;
  return neverd_func_entry(Sess, Index);
}

} // namespace

int runTypesFit(neverd_session_t Sess, raw_ostream &Out) {
  if (!TypeNames.empty() || TypesAll || TypesLayout || !TypesMember.empty()) {
    WithColor::error() << "--fit ranks types; leave out --type, --all, "
                          "--layout and --member\n";
    return 1;
  }
  json::Object Request{{"limit", static_cast<int64_t>(TypesLimit)}};
  if (TypesLimit < 1 ||
      TypesLimit >
          ::neverd::limits::cli::kNeverDCmdTypeFitRunTypesFitTypesLimit) {
    WithColor::error() << "--limit must be 1 to 500\n";
    return 1;
  }
  if (!TypesSpelling.empty())
    Request["spelling"] = TypesSpelling.getValue();
  if (!TypesMatch.empty())
    Request["text"] = TypesMatch.getValue();
  json::Array Accesses;
  for (const std::string &Text : TypesAccess) {
    SmallVector<StringRef, 3> Parts;
    StringRef(Text).split(Parts, ':');
    const std::optional<int64_t> Offset =
        Parts.size() >= 2 ? number(Parts[0]) : std::nullopt;
    const std::optional<int64_t> Size =
        Parts.size() >= 2 ? number(Parts[1]) : std::nullopt;
    if (!Offset || !Size || *Size < 0 || Parts.size() > 3) {
      WithColor::error() << "--access " << Text
                         << ": write offset:size or offset:size:use\n";
      return 1;
    }
    json::Object Access{{"offset", *Offset}, {"size", *Size}};
    if (Parts.size() == 3)
      Access["use"] = Parts[2].trim().str();
    Accesses.push_back(std::move(Access));
  }
  if (!Accesses.empty())
    Request["accesses"] = std::move(Accesses);
  if (!TypesAt.empty() && TypesParam >= 0) {
    WithColor::error() << "--at and --param name a pointer each; give one\n";
    return 1;
  }
  if (!TypesAt.empty() || TypesParam >= 0) {
    std::optional<uint64_t> At;
    if (!TypesAt.empty() && !(At = parseAddrArg(TypesAt))) {
      WithColor::error() << "--at needs an instruction's address (hex)\n";
      return 1;
    }
    if (TypesFunc.empty()) {
      WithColor::error()
          << (At ? "--at needs the --func the instruction is in\n"
                 : "--param needs the --func it is of\n");
      return 1;
    }
    const std::optional<uint64_t> Entry = functionEntry(Sess);
    if (!Entry) {
      WithColor::error() << "no function " << TypesFunc << "\n";
      return 1;
    }
    Request["function"] = "0x" + utohexstr(*Entry);
    Request["target"] =
        At ? json::Object{{"address", "0x" + utohexstr(*At)}}
           : json::Object{{"parameter", static_cast<int64_t>(TypesParam)}};
  } else if (!TypesFunc.empty()) {
    WithColor::error() << "--func names the function of --at or --param\n";
    return 1;
  }
  std::string Text;
  raw_string_ostream(Text) << json::Value(std::move(Request));
  std::optional<json::Value> Found =
      reply(Sess, neverd_local_types_fit_json(Sess, Text.c_str()), "types");
  if (!Found)
    return 1;
  if (JsonOutput) {
    Out << *Found << "\n";
    return 0;
  }
  const json::Object *Root = Found->getAsObject();
  if (!Root)
    return 1;
  if (const json::Object *Target = Root->getObject("target")) {
    Out << "Accesses through "
        << Target->getString("base").value_or("the pointer") << " in "
        << TypesFunc;
    if (const std::optional<int64_t> Parameter =
            Target->getInteger("parameter"))
      Out << ", its parameter " << *Parameter << ":\n";
    else
      Out << ", the instruction's at "
          << offsetText(Target->getInteger("offset").value_or(0)) << ":\n";
    if (const json::Array *List = Target->getArray("accesses"))
      for (const json::Value &Value : *List)
        if (const json::Object *Access = Value.getAsObject())
          Out << format(
              "  %-10s %3lld  %-8s at %s\n",
              offsetText(Access->getInteger("offset").value_or(0)).c_str(),
              static_cast<long long>(Access->getInteger("size").value_or(0)),
              Access->getString("use").value_or("").str().c_str(),
              Access->getString("address").value_or("").str().c_str());
    Out << "\n";
  }
  const json::Array *Judged = Root->getArray("accesses");
  const size_t Count = Judged ? Judged->size() : 0;
  const json::Array *Candidates = Root->getArray("candidates");
  Out << "Types that fit, best first (" << (Candidates ? Candidates->size() : 0)
      << " of " << Root->getInteger("total").value_or(0) << "):\n";
  if (!Candidates)
    return 0;
  for (size_t I = 0; I < Candidates->size(); ++I) {
    const json::Object *Candidate = (*Candidates)[I].getAsObject();
    if (!Candidate)
      continue;
    std::string Line =
        formatv("{0,3}. {1}  ({2}, 0x{3} bytes, {4})", I + 1,
                Candidate->getString("name").value_or(""),
                Candidate->getString("kind").value_or(""),
                utohexstr(Candidate->getInteger("size").value_or(0)),
                Candidate->getString("origin").value_or(""))
            .str();
    if (Count)
      Line += formatv("  {0} of {1} exact",
                      Candidate->getInteger("exact").value_or(0) +
                          Candidate->getInteger("exactElements").value_or(0),
                      Count)
                  .str();
    if (const int64_t Outside = Candidate->getInteger("outside").value_or(0))
      Line += formatv(", {0} outside", Outside).str();
    if (std::optional<int64_t> Shift = Candidate->getInteger("shift"))
      Line += formatv(", points at {0} ({1})",
                      Candidate->getString("shiftPath").value_or(""),
                      offsetText(*Shift))
                  .str();
    SmallVector<std::string, 4> Aliases;
    for (const char *Key : {"aliases", "pointerAliases"})
      if (const json::Array *List = Candidate->getArray(Key))
        for (const json::Value &Name : *List)
          Aliases.push_back(Name.getAsString().value_or("").str());
    if (!Aliases.empty())
      Line += "  also " + join(Aliases, ", ");
    if (std::optional<StringRef> Member = Candidate->getString("matchedMember"))
      Line += "  ." + Member->str();
    Out << Line << "\n";
    // The best one's landings, access by access.
    if (I != 0 || !Judged)
      continue;
    if (const json::Array *Fits = Candidate->getArray("fits"))
      for (size_t J = 0; J < Fits->size() && J < Judged->size(); ++J) {
        const json::Object *Fit = (*Fits)[J].getAsObject();
        const json::Object *Access = (*Judged)[J].getAsObject();
        if (!Fit || !Access)
          continue;
        Out << format(
            "       %-10s %3lld  %-8s %-8s %s\n",
            offsetText(Access->getInteger("offset").value_or(0)).c_str(),
            static_cast<long long>(Access->getInteger("size").value_or(0)),
            Fit->getString("fit").value_or("").str().c_str(),
            Fit->getString("path").value_or("").str().c_str(),
            Fit->getString("type").value_or("").str().c_str());
      }
  }
  return 0;
}

int runTypesMember(neverd_session_t Sess, raw_ostream &Out) {
  if (TypeNames.size() != 1 || TypesAll || TypesLayout) {
    WithColor::error() << "--member lands an access in one --type\n";
    return 1;
  }
  SmallVector<StringRef, 2> Parts;
  StringRef(TypesMember).split(Parts, ':');
  const std::optional<int64_t> Offset = number(Parts[0]);
  const std::optional<int64_t> Size =
      Parts.size() == 2 ? number(Parts[1]) : std::optional<int64_t>(0);
  if (!Offset || !Size || *Size < 0 || Parts.size() > 2) {
    WithColor::error() << "--member " << TypesMember
                       << ": write offset or offset:size\n";
    return 1;
  }
  std::string Options;
  if (!TypesSpelling.empty())
    raw_string_ostream(Options)
        << json::Value(json::Object{{"spelling", TypesSpelling.getValue()}});
  std::optional<json::Value> Found =
      reply(Sess,
            neverd_local_type_member_json(
                Sess, TypeNames.front().c_str(), *Offset,
                static_cast<unsigned long long>(*Size),
                Options.empty() ? nullptr : Options.c_str()),
            "types");
  if (!Found)
    return 1;
  if (JsonOutput) {
    Out << *Found << "\n";
    return 0;
  }
  const json::Object *At = Found->getAsObject();
  if (!At)
    return 1;
  const StringRef Record = At->getString("record").value_or("");
  const StringRef Path = At->getString("path").value_or("");
  const int64_t MemberOffset = At->getInteger("memberOffset").value_or(0);
  Out << Record << offsetText(*Offset) << " (" << *Size
      << " bytes): " << At->getString("fit").value_or("");
  if (!Path.empty())
    Out << " " << Record << "." << Path;
  if (*Offset != MemberOffset)
    Out << offsetText(*Offset - MemberOffset);
  if (std::optional<StringRef> Type = At->getString("type"))
    Out << "  (" << *Type << ", " << offsetText(MemberOffset) << ", "
        << At->getInteger("memberSize").value_or(0) << " bytes)";
  Out << "\n";
  return 0;
}

} // namespace neverd::cli
