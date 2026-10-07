//===- NeverDCmdXrefs.cpp - xrefs subcommand ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd xrefs`: the references to an address.  By default they are the
/// ones each instruction states and the pointers relocated slots hold, as
/// the GUI lists them, without analysis; --source=ir lists the constants of
/// the analyzed IR instead.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../NeverDCLIFunctions.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>
#include <vector>

using namespace llvm;

namespace neverd::cli {
namespace {

/// Functions one neverd_code_refs_json call decodes, and slots one
/// neverd_pointer_refs_json call reads.
constexpr int ReferencePageFunctions = 1024;
constexpr int ReferencePageSlots = 65536;

struct Reference {
  uint64_t From = 0;
  std::string Kind;
};

/// The classic one-letter type of a reference kind (ReferenceKinds.def).
char kindLetter(StringRef Kind) {
#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code)                          \
  if (Kind == Name)                                                            \
    return Letter;
#include "neverd/sdk/ReferenceKinds.def"
  return '?';
}

/// Appends the rows of one reference page aimed at \p Target; returns the
/// cursor of the next page, or none at the end or on an error.
std::optional<uint64_t> collectPage(const char *Page, StringRef NextKey,
                                    uint64_t Target,
                                    std::vector<Reference> &Out, bool &Failed) {
  if (!Page) {
    Failed = true;
    return std::nullopt;
  }
  std::string Text = Page;
  neverd_free_string(Page);
  auto Parsed = json::parse(Text);
  if (!Parsed || !Parsed->getAsObject()) {
    if (!Parsed)
      consumeError(Parsed.takeError());
    Failed = true;
    return std::nullopt;
  }
  const json::Object &Object = *Parsed->getAsObject();
  if (const json::Array *Rows = Object.getArray("refs"))
    for (const auto &Row : *Rows) {
      const json::Array *Fields = Row.getAsArray();
      uint64_t From = 0, To = 0;
      if (!Fields || Fields->size() != 3 ||
          (*Fields)[0].getAsString().value_or("").getAsInteger(0, From) ||
          (*Fields)[1].getAsString().value_or("").getAsInteger(0, To) ||
          To != Target)
        continue;
      Out.push_back({From, (*Fields)[2].getAsString().value_or("").str()});
    }
  uint64_t Next = 0;
  if (const auto Cursor = Object.getString(NextKey);
      Cursor && !Cursor->getAsInteger(0, Next))
    return Next;
  return std::nullopt;
}

int runDirectXrefs(neverd_session_t Sess, neverd_va_t Target) {
  std::vector<Reference> Refs;
  bool Failed = false;
  for (std::optional<uint64_t> Cursor = 0; Cursor && !Failed;)
    Cursor = collectPage(
        neverd_code_refs_json(Sess, *Cursor, ReferencePageFunctions),
        "next_entry", Target, Refs, Failed);
  for (std::optional<uint64_t> Cursor = 0; Cursor && !Failed;)
    Cursor =
        collectPage(neverd_pointer_refs_json(Sess, *Cursor, ReferencePageSlots),
                    "next_slot", Target, Refs, Failed);
  if (Failed) {
    WithColor::error() << takeLastError(Sess) << "\n";
    return 1;
  }
  std::stable_sort(
      Refs.begin(), Refs.end(),
      [](const Reference &A, const Reference &B) { return A.From < B.From; });
  const FunctionLocator Functions(Sess);
  if (JsonOutput) {
    json::Array Out;
    for (const Reference &R : Refs)
      Out.push_back(json::Object{{"from", "0x" + utohexstr(R.From)},
                                 {"to", "0x" + utohexstr(Target)},
                                 {"kind", R.Kind},
                                 {"type", std::string(1, kindLetter(R.Kind))},
                                 {"function", Functions.locate(R.From)}});
    outs() << json::Value(std::move(Out)) << "\n";
    return 0;
  }
  outs() << "XRefs to 0x" << utohexstr(Target) << ":\n";
  for (const Reference &R : Refs)
    outs() << format("  %-18s %c  ", ("0x" + utohexstr(R.From)).c_str(),
                     kindLetter(R.Kind))
           << Functions.locate(R.From) << "\n";
  outs() << Refs.size() << " references found\n";
  return 0;
}

int runIRXrefs(neverd_session_t Sess, neverd_va_t Target) {
  if (!JsonOutput) {
    outs() << "XRefs for 0x" << utohexstr(Target) << ":\n";
    outs() << "(requires pipeline - running lift...)\n";
  }

  const char *Json =
      neverd_xrefs_scan(Sess, InputFile.getValue().c_str(), Target);
  if (!Json) {
    WithColor::error() << "xrefs scan failed\n";
    return 1;
  }

  if (JsonOutput) {
    outs() << Json << "\n";
  } else {
    auto Parsed = json::parse(Json);
    size_t RefCount = 0;
    if (Parsed) {
      if (auto *Arr = Parsed->getAsArray()) {
        for (const auto &V : *Arr) {
          if (auto *Obj = V.getAsObject()) {
            outs() << "  " << Obj->getString("from").value_or("") << " in "
                   << Obj->getString("func").value_or("") << " (block "
                   << Obj->getInteger("block").value_or(0) << ")\n";
            ++RefCount;
          }
        }
      }
    }
    outs() << RefCount << " references found\n";
  }
  neverd_free_string(Json);
  return 0;
}

} // namespace

int runXrefs(neverd_session_t Sess) {
  neverd_va_t Target = 0;
  if (!parseHexAddress(XrefAddr.getValue(), Target)) {
    WithColor::error() << "invalid hexadecimal address\n";
    return 1;
  }
  return XrefSource == XrefSourceKind::IR ? runIRXrefs(Sess, Target)
                                          : runDirectXrefs(Sess, Target);
}

} // namespace neverd::cli
