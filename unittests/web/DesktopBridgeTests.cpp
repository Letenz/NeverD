//===- DesktopBridgeTests.cpp - Explicit desktop source bridge contracts --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Known direct syntax, redacted anchors, shadowing and unsupported dispatch.
///
//===----------------------------------------------------------------------===//

#include "desktop/bridge/Publication.h"
#include "gtest/gtest.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/DesktopBridge.h"

#include <cstdlib>
#include <fstream>

namespace {
using namespace neverd::web;
struct Inspection {
  std::string Text;
  SourceAnalysis S;
  SourceBindingAnalysis B;
  SourceNavigation N;
  DesktopBridgeSource A;
  explicit Inspection(std::string Bytes, std::string_view Mode = "script")
      : Text(std::move(Bytes)) {
    S = inspectJavaScript(identity("bridge-fixture", {Text}), Text, Mode);
    EXPECT_EQ(S.ParseStatus, "parsed");
    B = analyzeSourceBindings(S);
    N = analyzeSourceNavigation(S, B);
    A = analyzeDesktopBridgeSource(S, B, N);
  }
  std::string_view text(uint32_t Node) const {
    return std::string_view(Text).substr(
        S.Nodes.at(Node).Start, S.Nodes.at(Node).End - S.Nodes.at(Node).Start);
  }
};
} // namespace

TEST(WebDesktopBridge, StaticChainsCommandsAndCallbackValuesHaveExactAnchors) {
  Inspection I(
      R"JS(window.__TAURI_IPC__({cmd:'CANARY_COMMAND',callback:callback,error:error});
    window.__TAURI__.tauri.invoke('CANARY_COMMAND',{data:CANARY_DATA});
    window['go']['CANARY_PACKAGE'].CANARY_STRUCT.CANARY_METHOD(CANARY_ARG);
    window?.__TAURI__?.tauri.invoke?.(dynamicCommand);
    window.go.CANARY_PACKAGE.CANARY_STRUCT.CANARY_METHOD();
  )JS");
  ASSERT_EQ(I.A.Status, "partial");
  ASSERT_EQ(I.A.Calls.size(), 5u);
  const auto &C = I.A.Calls;
  EXPECT_EQ(C[0].Form, "window_ipc_message");
  EXPECT_EQ(C[0].CommandStatus, "literal_string_redacted");
  EXPECT_EQ(I.text(C[0].Callback), "callback");
  EXPECT_EQ(I.text(C[0].Error), "error");
  EXPECT_EQ(C[1].Form, "window_global_invoke");
  EXPECT_EQ(C[2].Form, "window_go_package_struct_method");
  EXPECT_EQ(C[2].CommandStatus, "static_member_names_redacted");
  EXPECT_EQ(I.text(C[2].TargetNames[1]), "CANARY_STRUCT");
  EXPECT_TRUE(C[3].Optional);
  EXPECT_EQ(C[3].CommandStatus, "dynamic_expression");
  for (const auto &R : C) {
    EXPECT_EQ(R.RootResolution, "external");
    const auto Row = desktop_bridge::record(R, I.S, I.B, I.N);
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    Out << llvm::json::Value(llvm::json::Object(Row));
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Row.getString("confidence"), "source_syntax_only");
    EXPECT_EQ(Row.getString("native_target"), "not_resolved");
  }
  const auto Again = analyzeDesktopBridgeSource(I.S, I.B, I.N);
  EXPECT_EQ(Again.ID, I.A.ID);
  for (size_t J = 0; J < C.size(); ++J)
    EXPECT_EQ(Again.Calls[J].ID, C[J].ID);
}
TEST(WebDesktopBridge,
     MessageSpreadsGettersDuplicatesAndArityKeepRolesUnknown) {
  Inspection I(R"JS(window.__TAURI_IPC__({cmd:'CANARY',...args});
    window.__TAURI_IPC__({cmd:'CANARY',cmd:other});
    window.__TAURI_IPC__({get cmd(){return 'CANARY'}});
    window.__TAURI_IPC__({[computed]:data,cmd:'CANARY'});
    window.__TAURI_IPC__(message);
    window.__TAURI_IPC__({});
    window.__TAURI__.tauri.invoke(...args);
    window.__TAURI__.tauri.invoke();
  )JS");
  ASSERT_EQ(I.A.Calls.size(), 8u);
  for (const auto &R : I.A.Calls) {
    EXPECT_EQ(R.Status, "unresolved_shape");
    EXPECT_EQ(R.CommandStatus, "unresolved_argument_shape");
    EXPECT_FALSE(R.Reason.empty());
  }
  EXPECT_EQ(I.A.Calls[0].Reason, "message_spread_or_unknown_property");
  EXPECT_EQ(I.A.Calls[1].Reason, "message_duplicate_role_property");
  EXPECT_EQ(I.A.Calls[2].Reason, "message_role_accessor_or_method");
  EXPECT_EQ(I.A.Calls[3].Reason, "message_computed_property");
}
TEST(WebDesktopBridge, LexicalDynamicRootsAndUnrecognizedDispatchStayDistinct) {
  Inspection I(R"JS(function f(window){window.__TAURI_IPC__({cmd:'CANARY'});}
    with(obj){window.__TAURI_IPC__({cmd:'CANARY'});}
    const invoke = window.__TAURI__.tauri.invoke; invoke('CANARY');
    window.__TAURI__[dynamic].invoke('CANARY');
    window.go[pkg].Struct.Method();
    window.go.Package.Method();
    unrelated.__TAURI_IPC__({cmd:'CANARY'});
  )JS");
  ASSERT_EQ(I.A.Calls.size(), 2u);
  EXPECT_EQ(I.A.Calls[0].RootResolution, "lexical_binding");
  EXPECT_EQ(I.A.Calls[1].RootResolution, "dynamic_with");
  const auto Summary = desktop_bridge::summary(I.A);
  EXPECT_EQ(Summary.getBoolean("bridge_inventory_complete"), false);
  EXPECT_EQ(Summary.getInteger("non_external_root_count"), 2);
}
TEST(WebDesktopBridge, RecordBudgetsAndInvalidModelsClearAllCandidates) {
  std::string Text;
  for (uint64_t I = 0; I <= MaxDesktopBridgeRecords; ++I)
    Text += "window.__TAURI_IPC__({cmd:'CANARY'});";
  Inspection I(std::move(Text));
  EXPECT_EQ(I.A.Status, "budget_exceeded");
  EXPECT_TRUE(I.A.Calls.empty());
  EXPECT_EQ(I.N.Status, "ok");
  Inspection Small("window.__TAURI_IPC__({cmd:'CANARY'});");
  Small.N.Calls[0].Callee = UINT32_MAX;
  const auto A = analyzeDesktopBridgeSource(Small.S, Small.B, Small.N);
  EXPECT_EQ(A.Status, "unavailable");
  EXPECT_TRUE(A.Calls.empty());

  Inspection Mixed("import('CANARY'); new unrelated(); tag`CANARY`;"
                   "window.__TAURI_IPC__({cmd:'CANARY'});");
  ASSERT_EQ(Mixed.A.Calls.size(), 1u);
  const auto Call = Mixed.A.Calls[0].Call;
  const auto Callee = Mixed.N.Calls[Call].Callee;
  Mixed.N.Calls[Call].Callee = Mixed.N.Calls[1].Callee;
  EXPECT_EQ(analyzeDesktopBridgeSource(Mixed.S, Mixed.B, Mixed.N).Reason,
            "desktop_bridge_invalid_navigation");
  Mixed.N.Calls[Call].Callee = Callee;
  Mixed.N.Calls[Call].Kind = "construct";
  EXPECT_EQ(analyzeDesktopBridgeSource(Mixed.S, Mixed.B, Mixed.N).Reason,
            "desktop_bridge_invalid_navigation");
}
TEST(WebDesktopBridge, OptionalCapturedTauriIPCMessageRetainsDynamicShape) {
  const auto *Path = std::getenv("NEVERD_TAURI_CHATGPT_110_X64_JS");
  if (!Path)
    GTEST_SKIP()
        << "preserved decoded Tauri JS qualification member unavailable";
  std::ifstream F(Path, std::ios::binary);
  Inspection I(std::string(std::istreambuf_iterator<char>(F), {}), "module");
  EXPECT_EQ(I.Text.size(), 85197u);
  EXPECT_EQ(I.S.BlobHash,
            "e22caa4af06a1d65ff970b54a842df47333ccdee0f1150487f48410bb953453f");
  ASSERT_EQ(I.A.Status, "partial") << I.A.Reason;
  ASSERT_EQ(I.A.Calls.size(), 1u);
  const auto &R = I.A.Calls[0];
  EXPECT_EQ(R.Form, "window_ipc_message");
  EXPECT_EQ(R.RootResolution, "external");
  EXPECT_EQ(R.Status, "unresolved_shape");
  EXPECT_EQ(R.Reason, "message_spread_or_unknown_property");
  EXPECT_EQ(R.CommandStatus, "unresolved_argument_shape");
  EXPECT_EQ(I.S.Nodes[R.Node].Start, 3013u);
  EXPECT_EQ(I.S.Nodes[R.Node].End - I.S.Nodes[R.Node].Start, 53u);
  RecordProperty("source_sha256", I.S.BlobHash);
  RecordProperty("call_byte_offset", std::to_string(I.S.Nodes[R.Node].Start));
  RecordProperty("call_byte_length", std::to_string(I.S.Nodes[R.Node].End -
                                                    I.S.Nodes[R.Node].Start));
}
