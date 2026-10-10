#include "HTMLFiles.h"

#include "neverd/web/SourceModules.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ConvertUTF.h"

namespace neverd::web {
SourceModuleLinks linkHTMLSourceModules(const SourceAnalysis &Source,
                                        const SourceModuleAnalysis &Modules,
                                        const HTMLDocument &Document,
                                        const HTMLLinks &Links,
                                        uint32_t ScriptIndex,
                                        const Snapshot &Input) {
  if (Modules.SourceID != Source.ID)
    throw Error("source_analysis_mismatch");
  if (Document.Status != "partial" ||
      Document.Scripts.size() > MaxHTMLRecords ||
      Document.Bases.size() > MaxHTMLRecords - Document.Scripts.size() ||
      ScriptIndex >= Document.Scripts.size() ||
      Links.ID !=
          identity("html-script-links", {Document.ID, Input.ID, HTMLProfile}))
    throw Error("invalid_html_module_context");
  const auto &Script = Document.Scripts[ScriptIndex];
  if (Script.InlineArtifactID.empty() ||
      Script.InlineArtifactID != Source.ArtifactID ||
      Script.BodyHash != Source.BlobHash || !Script.Closed ||
      Script.HasSource || Script.Context != "html_source_candidate" ||
      Script.BodyStatus != "raw_inline_source_candidate")
    throw Error("html_source_mismatch");
  SourceModuleLinks R;
  R.Profile = HTMLModuleLinkProfile;
  R.Context.emplace();
  auto &C = *R.Context;
  C.ID = identity("html-module-context", {HTMLModuleLinkProfile, Document.ID,
                                          Links.ID, Script.ID, Source.ID});
  C.Kind = "html_inline_source_candidate";
  C.HTMLID = Document.ID;
  C.ScriptID = Script.ID;
  C.ImportMapStatus = "not_analyzed";
  C.Status = "not_analyzed";
  R.ID =
      identity("source-module-links", {R.Profile, Modules.ID, Input.ID, C.ID});
  R.Status = "partial";
  if (Modules.Status != "ok" && Modules.Status != "partial") {
    R.Status = "unavailable";
    return R;
  }
  const auto Step = [&](uint64_t N = 1) {
    if (N > MaxHTMLModuleLinkSteps - R.Steps)
      throw Error("html_module_link_budget_exceeded");
    R.Steps += N;
  };
  try {
    if (Modules.Requests.size() > MaxHTMLModuleRequests)
      throw Error("html_module_link_budget_exceeded");
    const HTMLFiles Files(Input, Document.ArtifactID, Document.BlobHash, Step);
    auto Base = Files.documentBase();
    if (Script.Base != NoHTMLIndex) {
      if (Script.Base >= Document.Bases.size() ||
          !Document.Bases[Script.Base].Selected ||
          Document.Bases[Script.Base].Start >= Script.TagStart)
        throw Error("invalid_html_module_context");
      C.BaseID = Document.Bases[Script.Base].ID;
      Base = htmlDeclaredBase(Base, Document.Bases[Script.Base], Step);
    }
    C.Status = Base.Status;
    // Cached entry links and this context must describe the same base. A
    // separately exhausted entry link budget does not fabricate an origin.
    if (Links.Status == "partial") {
      if (Links.Scripts.size() != Document.Scripts.size())
        throw Error("invalid_html_module_context");
      const auto &Cached = Links.Scripts[ScriptIndex].Base;
      Step(Cached.Path.size() + Base.Path.size() + 1);
      if (Cached.Path != Base.Path || Cached.Status != Base.Status ||
          Cached.Directory != Base.Directory || Cached.Query != Base.Query ||
          Cached.Fragment != Base.Fragment)
        throw Error("invalid_html_module_context");
    } else if (Links.Status != "budget_exceeded")
      throw Error("invalid_html_module_context");
    bool HasImportMap = false;
    for (const auto &S : Document.Scripts) {
      Step();
      if (S.Kind == "importmap" && S.Context == "html_source_candidate")
        HasImportMap = true;
    }
    C.ImportMapStatus =
        HasImportMap ? "not_analyzed" : "no_eligible_import_map_declaration";
    if (Source.SourceType != Script.SourceType)
      C.Status = "html_source_type_mismatch";
    for (const auto &Request : Modules.Requests) {
      Step();
      SourceModuleLink L;
      if (Request.Kind != "esm_import" && Request.Kind != "esm_reexport" &&
          Request.Kind != "dynamic_import")
        L.Status = "unverified_callee";
      else if (Request.Specifier == NoSourceIndex)
        L.Status = "nonliteral_specifier";
      else {
        if (Request.Specifier >= Modules.Names.size())
          throw Error("invalid_source_module_model");
        const auto &Name = Modules.Names[Request.Specifier];
        Step(Name.size() + Base.Path.size() + 1);
        if (C.Status != "local_url_candidate")
          L.Status = C.Status;
        else if (C.ImportMapStatus == "not_analyzed")
          L.Status = "html_import_map_not_analyzed";
        else if (Name.size() > MaxJavaScriptModulePathUnits ||
                 !(Name.starts_with(u"./") || Name.starts_with(u"../")))
          L.Status = "unsupported_specifier";
        else {
          const std::vector<llvm::UTF16> Units(Name.begin(), Name.end());
          std::string Reference;
          if (!llvm::convertUTF16ToUTF8String(Units, Reference))
            L.Status = "unsupported_specifier";
          else {
            Step(Reference.size());
            const auto Target =
                resolveHTMLLocalURL(Base.Path, Base.Directory, Reference);
            if (Target.Status == "local_url_candidate") {
              L.Query = Target.Query;
              L.Fragment = Target.Fragment;
            }
            if (Target.Status != "local_url_candidate")
              L.Status = Target.Status;
            else if (Target.Directory)
              L.Status = "directory_target";
            else if (const auto *A = Files.find(Target.Path); !A)
              L.Status = "not_in_snapshot";
            else if (A->Directory)
              L.Status = "directory_target";
            else {
              L.Status = "exact_admitted_file_candidate";
              L.ArtifactID = A->ID;
            }
          }
        }
      }
      R.Requests.push_back(std::move(L));
    }
  } catch (const Error &E) {
    if (std::string_view(E.what()) != "html_module_link_budget_exceeded")
      throw;
    R.Status = "budget_exceeded";
    C.Status = "budget_exceeded";
    R.Requests.clear();
  }
  return R;
}
} // namespace neverd::web
