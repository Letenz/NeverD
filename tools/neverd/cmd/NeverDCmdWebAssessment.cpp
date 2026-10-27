//===- NeverDCmdWebAssessment.cpp - Selected offline package evidence -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compose retained evidence in one session and publish bounded report pages.
///
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"
#include "../ResourceLimits.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::cli {
namespace {
using Value = llvm::json::Value;
using Object = llvm::json::Object;
std::optional<Value> reply(const char *Owned) {
  if (!Owned) {
    llvm::errs() << "web: allocation_failed\n";
    return {};
  }
  auto V = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  if (!V) {
    llvm::consumeError(V.takeError());
    llvm::errs() << "web: invalid_backend_response\n";
    return {};
  }
  if (!V->getAsObject() || V->getAsObject()->getString("status") != "ok") {
    llvm::outs() << *V << '\n';
    return {};
  }
  return std::move(*V);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
bool index(llvm::StringRef Text, uint64_t &Number) {
  return !Text.empty() && !Text.getAsInteger(10, Number) &&
         std::to_string(Number) == Text;
}
struct FeedSelection {
  uint64_t Input = 0, Descriptor = 0, Original = 0;
};
struct SourceSelection {
  uint64_t Index = 0;
  std::string Type;
};
struct Arguments {
  bool Archive = false, Markdown = false;
  uint64_t Input = 0, Manifest = 0;
  std::string Path, Format, Kind;
  std::optional<FeedSelection> Registry;
  std::vector<FeedSelection> Advisories;
  std::vector<SourceSelection> Sources;
};
std::optional<Arguments> arguments() {
  Arguments A;
  A.Archive = WebArguments[0] == "archive-assessment";
  const size_t Base = A.Archive ? 6 : 4;
  if (WebArguments.size() < Base)
    return {};
  A.Path = WebArguments[1];
  if (A.Archive) {
    A.Format = WebArguments[2];
    if ((A.Format != "tar" && A.Format != "tgz" && A.Format != "zip") ||
        !index(WebArguments[3], A.Input) || !index(WebArguments[4], A.Manifest))
      return {};
  } else if (!index(WebArguments[2], A.Manifest))
    return {};
  A.Kind = WebArguments[Base - 1];
  if (A.Kind != "npm-lock" && A.Kind != "package-json")
    return {};
  for (size_t I = Base; I < WebArguments.size();) {
    const auto &Flag = WebArguments[I++];
    if (Flag == "--markdown") {
      if (A.Markdown)
        return {};
      A.Markdown = true;
    } else if (Flag == "--registry" || Flag == "--advisory") {
      const bool Registry = Flag == "--registry";
      const size_t Count = Registry ? 3 : 2;
      FeedSelection F;
      if (I + Count > WebArguments.size() || !index(WebArguments[I], F.Input) ||
          !index(WebArguments[I + 1], F.Descriptor) ||
          (Registry && !index(WebArguments[I + 2], F.Original)))
        return {};
      if (Registry) {
        if (A.Registry)
          return {};
        A.Registry = F;
      } else {
        if (A.Advisories.size() ==
            ::neverd::limits::cli::kMaxCLIWebAssessmentSelections)
          return {};
        A.Advisories.push_back(F);
      }
      I += Count;
    } else if (Flag == "--source") {
      if (I == WebArguments.size() ||
          A.Sources.size() ==
              ::neverd::limits::cli::kMaxCLIWebAssessmentSelections)
        return {};
      const auto [Number, Type] = llvm::StringRef(WebArguments[I++]).split(':');
      SourceSelection S;
      if (!index(Number, S.Index) ||
          (Type != "script" && Type != "module" && Type != "commonjs"))
        return {};
      S.Type = Type.str();
      A.Sources.push_back(std::move(S));
    } else
      return {};
  }
  return A;
}
struct Client {
  neverd_web_session_t S = neverd_web_session_create();
  std::string Revision, ArchiveID;
  ~Client() { neverd_web_session_destroy(S); }
  std::string artifact(uint64_t Index, bool Member = false) {
    const auto P =
        reply(Member ? neverd_web_package_archive_records_json(
                           S, Revision.data(), Revision.size(),
                           ArchiveID.data(), ArchiveID.size(), Index, 1)
                     : neverd_web_artifacts_json(S, Revision.data(),
                                                 Revision.size(), Index, 1));
    const auto *Items = P ? P->getAsObject()->getArray("items") : nullptr;
    return Items && Items->size() == 1
               ? field((*Items)[0], Member ? "member_id" : "artifact_id")
               : std::string();
  }
  bool open(const Arguments &A) {
    if (!S)
      return false;
    const auto P = reply(neverd_web_import_preview_json(
        S, A.Path.data(), A.Path.size(), nullptr, 0));
    if (!P)
      return false;
    const auto Token = field(*P, "preview_token");
    const auto C =
        reply(neverd_web_import_commit_json(S, Token.data(), Token.size()));
    if (!C)
      return false;
    Revision = field(*C, "revision");
    if (A.Archive) {
      const auto Original = artifact(A.Input);
      const auto Extracted = reply(neverd_web_package_archive_extract_json(
          S, Revision.data(), Revision.size(), Original.data(), Original.size(),
          A.Format.data(), A.Format.size()));
      if (!Extracted)
        return false;
      ArchiveID = field(*Extracted, "archive_id");
    }
    return true;
  }
  bool metadata(const Arguments &A, const std::string &Packages, Object &O) {
    if (A.Registry) {
      const auto Input = artifact(A.Registry->Input),
                 Descriptor = artifact(A.Registry->Descriptor),
                 Original = artifact(A.Registry->Original);
      const auto T = reply(neverd_web_package_attestation_import_json(
          S, Revision.data(), Revision.size(), Input.data(), Input.size(),
          Descriptor.data(), Descriptor.size(), "registry-dist", 13));
      if (!T)
        return false;
      const auto ID = field(*T, "package_attestation_id");
      O["attestation_ids"] = llvm::json::Array{ID};
      const auto I = reply(neverd_web_package_integrity_verify_json(
          S, Revision.data(), Revision.size(), Original.data(), Original.size(),
          Input.data(), Input.size(), nullptr, 0));
      if (!I)
        return false;
      O["integrity_ids"] = llvm::json::Array{field(*I, "integrity_id")};
    }
    if (!A.Advisories.empty()) {
      llvm::json::Array IDs;
      for (const auto &F : A.Advisories) {
        const auto Input = artifact(F.Input),
                   Descriptor = artifact(F.Descriptor);
        const auto R = reply(neverd_web_package_advisory_import_json(
            S, Revision.data(), Revision.size(), Input.data(), Input.size(),
            Descriptor.data(), Descriptor.size()));
        if (!R)
          return false;
        IDs.emplace_back(field(*R, "package_advisory_id"));
      }
      const auto Selection =
          llvm::formatv("{0}", Value(Object{{"schema_version", 1},
                                            {"advisory_ids", std::move(IDs)}}))
              .str();
      const auto M = reply(neverd_web_package_advisories_match_json(
          S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
          Selection.data(), Selection.size()));
      if (!M)
        return false;
      O["advisory_matches_id"] = field(*M, "package_advisory_matches_id");
    }
    return true;
  }
  bool behavior(const Arguments &A, const std::string &Packages, Object &O) {
    if (A.Sources.empty())
      return true;
    llvm::json::Array Sources;
    for (const auto &Selected : A.Sources) {
      const auto Artifact = artifact(Selected.Index, A.Archive);
      const auto R = reply(neverd_web_source_analyze_json(
          S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
          Selected.Type.data(), Selected.Type.size()));
      if (!R)
        return false;
      Sources.emplace_back(field(*R, "source_id"));
    }
    const auto Selection =
        llvm::formatv("{0}", Value(Object{{"schema_version", 1},
                                          {"source_ids", std::move(Sources)}}))
            .str();
    const auto B = reply(neverd_web_package_behavior_analyze_json(
        S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
        Selection.data(), Selection.size()));
    if (!B)
      return false;
    const auto BehaviorID = field(*B, "package_behavior_id");
    O["package_behavior_id"] = BehaviorID;
    const auto InstallOptions =
        llvm::formatv("{0}", Value(Object{{"schema_version", 1},
                                          {"package_behavior_id", BehaviorID}}))
            .str();
    const auto I = reply(neverd_web_package_install_analyze_json(
        S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
        InstallOptions.data(), InstallOptions.size()));
    if (!I)
      return false;
    O["package_install_id"] = field(*I, "package_install_id");
    return true;
  }
};
} // namespace

int runWebAssessment() {
  const auto A = arguments();
  if (!A) {
    llvm::errs()
        << "usage: neverd web package-assessment <root> <manifest-index> "
           "<npm-lock|package-json> | archive-assessment <root> "
           "<tar|tgz|zip> <archive-index> <manifest-member-index> "
           "<npm-lock|package-json> "
           "[-- --registry metadata-index descriptor-index original-index] "
           "[--advisory metadata-index descriptor-index] "
           "[--source index:script|module|commonjs] [--markdown]\n";
    return 2;
  }
  Client C;
  if (!C.open(*A))
    return 1;
  const auto Manifest = C.artifact(A->Manifest, A->Archive);
  const auto P = reply(neverd_web_packages_analyze_json(
      C.S, C.Revision.data(), C.Revision.size(), Manifest.data(),
      Manifest.size(), A->Kind.data(), A->Kind.size()));
  if (!P)
    return 1;
  const auto Packages = field(*P, "package_analysis_id");
  Object O{{"schema_version", 1}};
  if (!C.metadata(*A, Packages, O) || !C.behavior(*A, Packages, O))
    return 1;
  const auto Options = llvm::formatv("{0}", Value(std::move(O))).str();
  const auto Assessment = reply(neverd_web_package_assessment_create_json(
      C.S, C.Revision.data(), C.Revision.size(), Packages.data(),
      Packages.size(), Options.data(), Options.size()));
  if (!Assessment)
    return 1;
  const auto ID = field(*Assessment, "package_assessment_id"),
             Head = field(*Assessment, "review_head_id");
  std::vector<std::string> Output;
  uint64_t Bytes = 0, Offset = 0;
  if (!A->Markdown) {
    Output.push_back(llvm::formatv("{0}\n", *Assessment).str());
    Bytes = Output.back().size();
  }
  for (;;) {
    const auto Call = A->Markdown ? neverd_web_package_assessment_report_json
                                  : neverd_web_package_assessment_findings_json;
    const auto Page =
        reply(Call(C.S, C.Revision.data(), C.Revision.size(), ID.data(),
                   ID.size(), Head.data(), Head.size(), Offset, 512));
    if (!Page)
      return 1;
    Output.push_back(A->Markdown ? field(*Page, "report_markdown")
                                 : llvm::formatv("{0}\n", *Page).str());
    Bytes += Output.back().size();
    if (Bytes > ::neverd::limits::cli::
                    kNeverDCmdWebAssessmentRunWebAssessmentBytesLimit)
      return 1;
    const auto Next = Page->getAsObject()->getInteger("next_offset");
    if (!Next) {
      if (Page->getAsObject()->getBoolean("page_complete") != true)
        return 1;
      break;
    }
    if (*Next < 0 || uint64_t(*Next) <= Offset ||
        *Next > ::neverd::limits::cli::kMaxWebAssessmentOffset)
      return 1;
    Offset = *Next;
  }
  for (const auto &Text : Output)
    llvm::outs() << Text;
  return 0;
}
} // namespace neverd::cli
