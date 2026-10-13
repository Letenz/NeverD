//===- PassiveStructureTests.cpp - Observed field evidence witnesses -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent nesting, presence, grouping and private-key regression cases.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/interfaces/report/Publication.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/PassiveReport.h"

#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace neverd::web;
namespace {
struct Example {
  std::string Body;
  std::string URL = "https://CANARY.invalid/a";
  std::string Method = "POST";
  bool Response = false;
};

struct Observed {
  HARCapture Capture;
  std::vector<PassiveReportRecord> Records;
  explicit Observed(const std::vector<Example> &Examples) {
    llvm::json::Array Entries;
    for (const auto &E : Examples) {
      llvm::json::Object Body{{"mimeType", "application/json"},
                              {"text", E.Body}};
      llvm::json::Object Request{{"method", E.Method}, {"url", E.URL}};
      llvm::json::Object Response{{"status", 200}};
      if (E.Response)
        Response["content"] = std::move(Body);
      else
        Request["postData"] = std::move(Body);
      Entries.emplace_back(llvm::json::Object{
          {"request", std::move(Request)}, {"response", std::move(Response)}});
    }
    std::string Bytes;
    llvm::raw_string_ostream(Bytes) << llvm::json::Value(llvm::json::Object{
        {"log", llvm::json::Object{{"version", "1.2"},
                                   {"entries", std::move(Entries)}}}});
    Capture = inspectHAR("a", Bytes);
    Records = inspectPassiveHARShapes(Capture, Bytes);
  }
  std::vector<const PassiveReportRecord *>
  children(std::string_view Parent, std::string_view Anchor) const {
    std::vector<const PassiveReportRecord *> R;
    for (const auto &N : Records)
      if (N.Kind == "observed_json_value_node" && N.AnchorID == Anchor &&
          N.Structure->ParentNodeID == Parent)
        R.push_back(&N);
    return R;
  }
  const PassiveReportRecord *schema(std::string_view ID) const {
    for (const auto &R : Records)
      if (R.Kind == "observed_json_schema_node" && R.Structure->NodeID == ID)
        return &R;
    return nullptr;
  }
};
} // namespace

TEST(WebPassiveStructure, EqualHistogramsRetainDifferentNestedFieldTrees) {
  const Observed O({{R"({"left":{"x":1},"right":true})"},
                    {R"({"left":1,"right":{"x":true}})"}});
  const auto &First = O.Capture.Observations[0].RequestID;
  const auto &Second = O.Capture.Observations[1].RequestID;
  const auto RootsA = O.children("", First), RootsB = O.children("", Second);
  ASSERT_EQ(RootsA.size(), 1u);
  ASSERT_EQ(RootsB.size(), 1u);
  const auto A = O.children(RootsA[0]->Structure->NodeID, First);
  const auto B = O.children(RootsB[0]->Structure->NodeID, Second);
  ASSERT_EQ(A.size(), 2u);
  ASSERT_EQ(B.size(), 2u);
  EXPECT_EQ(A[0]->ShapeKind, "object");
  EXPECT_EQ(A[1]->ShapeKind, "boolean");
  EXPECT_EQ(B[0]->ShapeKind, "number");
  EXPECT_EQ(B[1]->ShapeKind, "object");
  EXPECT_EQ(A[0]->Structure->SchemaNodeID, B[0]->Structure->SchemaNodeID);
  EXPECT_EQ(A[1]->Structure->SchemaNodeID, B[1]->Structure->SchemaNodeID);
  const auto AX = O.children(A[0]->Structure->NodeID, First);
  const auto BX = O.children(B[1]->Structure->NodeID, Second);
  ASSERT_EQ(AX.size(), 1u);
  ASSERT_EQ(BX.size(), 1u);
  EXPECT_EQ(AX[0]->ShapeKind, "number");
  EXPECT_EQ(BX[0]->ShapeKind, "boolean");
  EXPECT_NE(AX[0]->Structure->SchemaNodeID, BX[0]->Structure->SchemaNodeID);
  const auto *Left = O.schema(A[0]->Structure->SchemaNodeID);
  ASSERT_NE(Left, nullptr);
  EXPECT_EQ(Left->Types,
            (std::map<std::string, uint64_t>{{"object", 1}, {"number", 1}}));
  EXPECT_EQ(Left->Structure->Instances, 2u);
  EXPECT_EQ(Left->Structure->EligibleParents, 2u);
  const auto *Child = O.schema(AX[0]->Structure->SchemaNodeID);
  ASSERT_NE(Child, nullptr);
  EXPECT_EQ(Child->Structure->EligibleParents, 1u);
  EXPECT_EQ(Child->Structure->MissingInstances, 0u);
}

TEST(WebPassiveStructure, MissingNullAndMixedArrayParentsRemainDistinct) {
  const Observed O({{R"({"CANARY_NAME":null})"},
                    {"{}"},
                    {R"({"CANARY_NAME":0})"},
                    {R"([{"CANARY_NAME":true},{},null])"},
                    {"{malformed"}});
  uint64_t Fields = 0;
  for (const auto &R : O.Records) {
    if (R.Kind != "observed_json_schema_node" ||
        R.Structure->Edge != "object_field")
      continue;
    ++Fields;
    EXPECT_EQ(R.Structure->MissingInstances, 1u);
    if (R.Types.count("null")) {
      EXPECT_EQ(R.Structure->EligibleParents, 3u);
      EXPECT_EQ(R.Structure->Instances, 2u);
      EXPECT_EQ(R.Types,
                (std::map<std::string, uint64_t>{{"null", 1}, {"number", 1}}));
    } else {
      EXPECT_EQ(R.Structure->EligibleParents, 2u);
      EXPECT_EQ(R.Structure->Instances, 1u);
      EXPECT_EQ(R.Types, (std::map<std::string, uint64_t>{{"boolean", 1}}));
    }
  }
  EXPECT_EQ(Fields, 2u);
  const auto &ArrayID = O.Capture.Observations[3].RequestID;
  const auto Roots = O.children("", ArrayID);
  ASSERT_EQ(Roots.size(), 1u);
  const auto Items = O.children(Roots[0]->Structure->NodeID, ArrayID);
  ASSERT_EQ(Items.size(), 3u);
  for (uint64_t I = 0; I < Items.size(); ++I) {
    EXPECT_EQ(Items[I]->Structure->Ordinal, I);
    EXPECT_EQ(Items[I]->Structure->SchemaNodeID,
              Items[0]->Structure->SchemaNodeID);
  }
}

TEST(WebPassiveStructure, GroupsNeverMergeUnknownMethodsOrDistinctEndpoints) {
  const Observed O({{"{}", "https://CANARY.invalid/a", "POST"},
                    {"{}", "https://CANARY.invalid/a?ignored=1", "POST"},
                    {"{}", "https://CANARY.invalid/b", "POST"},
                    {"{}", "https://CANARY.invalid/a", "GET"},
                    {"{}", "https://CANARY.invalid/a", "CUSTOM"},
                    {"{}", "https://CANARY.invalid/a", "CUSTOM"},
                    {"{}", "/relative", "POST"},
                    {"{}", "/relative", "POST"},
                    {"{}", "https://CANARY.invalid/a", "POST", true}});
  std::set<std::string> Groups;
  uint64_t Roots = 0;
  for (const auto &R : O.Records) {
    if (R.Kind != "observed_json_schema_node" || R.Structure->Edge != "root")
      continue;
    Groups.insert(R.Structure->GroupID);
    ++Roots;
  }
  EXPECT_EQ(Groups.size(), 8u);
  EXPECT_EQ(Roots, 8u);
}

TEST(WebPassiveStructure, PublicIDsAreUniqueAndAllNamesAndValuesStayPrivate) {
  const Observed O({{R"({"CANARY_NAME":{"CANARY_CHILD":"CANARY_VALUE"}})"},
                    {R"({"CANARY_NAME":null})"}});
  const auto Report =
      composePassiveReport(nullptr, nullptr, &O.Capture, O.Records, nullptr);
  std::set<std::string> IDs, Nodes;
  for (const auto &R : Report.Records) {
    EXPECT_TRUE(IDs.insert(R.ID).second);
    if (R.Structure && !R.Structure->NodeID.empty())
      EXPECT_TRUE(Nodes.insert(R.Structure->NodeID).second);
    std::string Text;
    llvm::raw_string_ostream(Text)
        << llvm::json::Value(passive_report::record(R));
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_FALSE(R.CoordinatesPresent);
  }
  for (const auto &R : Report.Records)
    if (R.Structure && !R.Structure->ParentNodeID.empty())
      EXPECT_TRUE(Nodes.count(R.Structure->ParentNodeID));
}

TEST(WebPassiveStructure, ObjectOrderDoesNotChangeCaptureLocalFieldIdentity) {
  const Observed O({{R"({"z":true,"a":null})"}, {R"({"a":null,"z":true})"}});
  std::vector<std::vector<const PassiveReportRecord *>> Fields;
  for (const auto &Example : O.Capture.Observations) {
    const auto Roots = O.children("", Example.RequestID);
    ASSERT_EQ(Roots.size(), 1u);
    Fields.push_back(
        O.children(Roots[0]->Structure->NodeID, Example.RequestID));
  }
  ASSERT_EQ(Fields[0].size(), 2u);
  ASSERT_EQ(Fields[1].size(), 2u);
  for (unsigned I = 0; I != 2; ++I) {
    EXPECT_EQ(Fields[0][I]->ShapeKind, Fields[1][I]->ShapeKind);
    EXPECT_EQ(Fields[0][I]->Structure->SchemaNodeID,
              Fields[1][I]->Structure->SchemaNodeID);
  }
}

TEST(WebPassiveStructure, PrivateFieldDictionariesHaveAnAggregateByteBudget) {
  std::vector<Example> Examples;
  for (unsigned I = 0; I != 110; ++I)
    Examples.push_back(
        {"{\"" + std::to_string(I) + std::string(20000, 'x') + "\":null}"});
  try {
    Observed O(Examples);
    FAIL() << "Oversized private dictionary admitted";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), "passive_shape_private_budget_exceeded");
  }
}
