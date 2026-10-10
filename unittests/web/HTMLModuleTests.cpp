#include "gtest/gtest.h"

#include "neverd/web/HTML.h"
#include "neverd/web/Session.h"
#include "neverd/web/SourceModules.h"

namespace {
using namespace neverd::web;
struct Fixture {
  std::string Text;
  HTMLDocument Document;
  Snapshot Input;
  HTMLLinks Entries;
  SourceAnalysis Source;
  SourceModuleAnalysis Modules;
  uint32_t Index;
  Fixture(std::string Text, uint32_t Index = 0)
      : Text(std::move(Text)), Index(Index) {
    Document = inspectHTML("document", this->Text);
    Input.ID = "captured-html-namespace";
    Input.Artifacts = {
        Artifact{"root", {}, {}, {}, "directory", {}, true},
        Artifact{"document", sha256(this->Text), "root", "app/index.html"},
        Artifact{"document-dep", {}, "root", "app/dep.js"},
        Artifact{"base-dep", {}, "root", "assets/dep.js"},
        Artifact{"unicode-dep", {}, "root", "assets/中.js"},
        Artifact{"directory", {}, "root", "assets/dir", "directory", {}, true}};
    Entries = linkHTMLScripts(Document, Input);
    const auto &S = Document.Scripts.at(Index);
    Source = inspectJavaScript(
        S.InlineArtifactID,
        this->Text.substr(S.BodyStart, S.BodyEnd - S.BodyStart), S.SourceType);
    Modules = analyzeSourceModules(Source, analyzeSourceBindings(Source));
  }
  SourceModuleLinks links() const {
    return linkHTMLSourceModules(Source, Modules, Document, Entries, Index,
                                 Input);
  }
};
TEST(WebHTMLModules,
     InlineImportsUseThePrecedingDocumentBaseAndRetainEvidence) {
  Fixture F("<base href='../assets/'><script type=module>import './dep.js'; "
            "export * from './dep.js';</script>");
  ASSERT_EQ(F.Modules.Status, "ok");
  const auto L = F.links();
  ASSERT_EQ(L.Requests.size(), 2u);
  EXPECT_EQ(L.Profile, HTMLModuleLinkProfile);
  EXPECT_EQ(L.Status, "partial");
  for (const auto &R : L.Requests) {
    EXPECT_EQ(R.Status, "exact_admitted_file_candidate");
    EXPECT_EQ(R.ArtifactID, "base-dep");
  }
  ASSERT_TRUE(L.Context);
  EXPECT_EQ(L.Context->HTMLID, F.Document.ID);
  EXPECT_EQ(L.Context->ScriptID, F.Document.Scripts[0].ID);
  EXPECT_EQ(L.Context->BaseID, F.Document.Bases[0].ID);
  EXPECT_EQ(L.Context->Status, "local_url_candidate");
  EXPECT_EQ(L.ID, F.links().ID);
  Fixture Later("<script type=module>import './dep.js';</script><base "
                "href='../assets/'>");
  EXPECT_EQ(Later.links().Requests[0].ArtifactID, "document-dep");
  EXPECT_TRUE(Later.links().Context->BaseID.empty());
  EXPECT_NE(L.ID, Later.links().ID);
}
TEST(WebHTMLModules, PercentEncodedURLsKeepQueryAndFragmentPresenceOnly) {
  Fixture F("<base href='../assets/'><script type=module>import "
            "'./%E4%B8%AD.js?CANARY#secret'; import "
            "'./dep.js#fragment?not-query';</script>");
  const auto L = F.links();
  ASSERT_EQ(L.Requests.size(), 2u);
  EXPECT_EQ(L.Requests[0].ArtifactID, "unicode-dep");
  EXPECT_EQ(L.Requests[0].Query, true);
  EXPECT_EQ(L.Requests[0].Fragment, true);
  EXPECT_EQ(L.Requests[1].ArtifactID, "base-dep");
  EXPECT_EQ(L.Requests[1].Query, false);
  EXPECT_EQ(L.Requests[1].Fragment, true);
}
TEST(WebHTMLModules,
     LiteralDynamicImportsRemainCandidatesAndCallsStayUnverified) {
  Fixture F("<script>import('./dep.js'); import(name); "
            "require('./dep.js');</script>");
  const auto L = F.links();
  ASSERT_EQ(L.Requests.size(), 3u);
  EXPECT_EQ(F.Modules.Requests[0].Kind, "dynamic_import");
  EXPECT_EQ(L.Requests[0].ArtifactID, "document-dep");
  EXPECT_EQ(L.Requests[1].Status, "nonliteral_specifier");
  EXPECT_EQ(L.Requests[2].Status, "unverified_callee");
  auto WrongType = F.Source;
  WrongType.SourceType = "module";
  const auto Wrong = linkHTMLSourceModules(WrongType, F.Modules, F.Document,
                                           F.Entries, F.Index, F.Input);
  EXPECT_EQ(Wrong.Context->Status, "html_source_type_mismatch");
  EXPECT_EQ(Wrong.Requests[0].Status, "html_source_type_mismatch");
}
TEST(WebHTMLModules, UnsupportedURLsAndMissingTargetsCannotInventMembers) {
  Fixture F("<base href='../assets/'><script type=module>import "
            "'../../escape.js'; import './dir/'; import './missing.js'; import "
            "'/dep.js'; import 'pkg'; import 'https://CANARY.invalid/x'; "
            "import './%2fdep.js'; import './\\uD800';</script>");
  const auto L = F.links();
  ASSERT_EQ(L.Requests.size(), 8u);
  EXPECT_EQ(L.Requests[0].Status, "outside_snapshot_root");
  EXPECT_EQ(L.Requests[1].Status, "directory_target");
  EXPECT_EQ(L.Requests[2].Status, "not_in_snapshot");
  for (const auto &R : L.Requests)
    EXPECT_TRUE(R.ArtifactID.empty());
  Fixture Remote("<base href='https://CANARY.invalid/'><script "
                 "type=module>import './dep.js';</script>");
  EXPECT_EQ(Remote.links().Requests[0].Status, "unsupported_local_url");
  F.Input.Artifacts.erase(F.Input.Artifacts.begin());
  F.Input.Artifacts[0].MemberPath.clear();
  F.Entries = linkHTMLScripts(F.Document, F.Input);
  EXPECT_EQ(F.links().Requests[0].Status, "directory_origin_unavailable");
}
TEST(WebHTMLModules, ImportMapsPreventAssumingTheDefaultURLRule) {
  for (const auto Markup :
       {"<script "
        "type=importmap>{\"imports\":{\"./dep.js\":null}}</script><script "
        "type=module>import './dep.js';</script>",
        "<script type=module>import './dep.js';</script><script "
        "type=importmap>{}</script>"}) {
    const uint32_t Index =
        std::string_view(Markup).starts_with("<script type=importmap") ? 1 : 0;
    Fixture F(Markup, Index);
    const auto L = F.links();
    ASSERT_EQ(L.Requests.size(), 1u);
    EXPECT_EQ(L.Context->ImportMapStatus, "not_analyzed");
    EXPECT_EQ(L.Requests[0].Status, "html_import_map_not_analyzed");
    EXPECT_TRUE(L.Requests[0].ArtifactID.empty());
  }
  Fixture Inert(
      "<template><script type=importmap>{}</script></template><script "
      "type=module>import './dep.js';</script>",
      1);
  EXPECT_EQ(Inert.links().Requests[0].ArtifactID, "document-dep");
}
TEST(WebHTMLModules, ContextAndSourceMismatchesRefuseBeforePublication) {
  Fixture F("<script type=module>import './dep.js';</script>");
  auto Wrong = F.Source;
  Wrong.BlobHash = "wrong";
  EXPECT_THROW(linkHTMLSourceModules(Wrong, F.Modules, F.Document, F.Entries, 0,
                                     F.Input),
               Error);
  EXPECT_THROW(linkHTMLSourceModules(F.Source, F.Modules, F.Document, F.Entries,
                                     1, F.Input),
               Error);
  auto Entries = F.Entries;
  Entries.Scripts[0].Base.Path = "other/path.html";
  EXPECT_THROW(linkHTMLSourceModules(F.Source, F.Modules, F.Document, Entries,
                                     0, F.Input),
               Error);
  auto Other = F.Input;
  Other.ID = "other-namespace";
  EXPECT_THROW(linkHTMLSourceModules(F.Source, F.Modules, F.Document, F.Entries,
                                     0, Other),
               Error);
  Other = F.Input;
  Other.Artifacts.push_back(Other.Artifacts[2]);
  EXPECT_THROW(linkHTMLSourceModules(F.Source, F.Modules, F.Document, F.Entries,
                                     0, Other),
               Error);
  Entries = F.Entries;
  Entries.Status = "budget_exceeded";
  Entries.Scripts.clear();
  EXPECT_EQ(linkHTMLSourceModules(F.Source, F.Modules, F.Document, Entries, 0,
                                  F.Input)
                .Requests[0]
                .ArtifactID,
            "document-dep");
}
TEST(WebHTMLModules, UnavailableInventoryDoesNotClaimImportMapAbsence) {
  Fixture F("<script type=importmap>{}</script><script "
            "type=module>import;</script>",
            1);
  ASSERT_NE(F.Source.ParseStatus, "parsed");
  ASSERT_EQ(F.Modules.Status, "unavailable");
  const auto L = F.links();
  EXPECT_EQ(L.Status, "unavailable");
  EXPECT_TRUE(L.Requests.empty());
  ASSERT_TRUE(L.Context);
  EXPECT_EQ(L.Context->Status, "not_analyzed");
  EXPECT_EQ(L.Context->ImportMapStatus, "not_analyzed");
}
TEST(WebHTMLModules, WorkAndRequestLimitsClearAllCandidateLinks) {
  std::string Text = "<script type=module>";
  for (unsigned I = 0; I < 1000; ++I)
    Text += "import './dep.js';";
  Text += "</script>";
  Fixture F(Text);
  ASSERT_EQ(F.Modules.Requests.size(), 1000u);
  std::string Path;
  for (unsigned I = 0; I < 15; ++I)
    Path += std::string(250, 'x') + '/';
  F.Input.Artifacts[1].MemberPath = Path + "index.html";
  F.Entries = linkHTMLScripts(F.Document, F.Input);
  const auto Work = F.links();
  EXPECT_EQ(Work.Status, "budget_exceeded");
  EXPECT_TRUE(Work.Requests.empty());
  EXPECT_LE(Work.Steps, MaxHTMLModuleLinkSteps);
  F.Modules.Requests.resize(MaxHTMLModuleRequests + 1, F.Modules.Requests[0]);
  const auto Count = F.links();
  EXPECT_EQ(Count.Status, "budget_exceeded");
  EXPECT_TRUE(Count.Requests.empty());
  ASSERT_TRUE(Count.Context);
  EXPECT_EQ(Count.Context->ImportMapStatus, "not_analyzed");
}
} // namespace
