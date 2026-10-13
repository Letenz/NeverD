//===- PassiveReportTests.cpp - Observed shape report boundaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent JSON type answers, explicit selection and atomic budgets.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/interfaces/report/Publication.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/PassiveReport.h"

#include "llvm/Support/raw_ostream.h"

using namespace neverd::web;
namespace {
std::string har(const llvm::json::Object &Body) {
  std::string Text;
  llvm::json::Array Entries;
  Entries.emplace_back(llvm::json::Object{
      {"request", llvm::json::Object{{"method", "POST"},
                                     {"url", "https://CANARY.invalid/a"},
                                     {"postData", llvm::json::Object(Body)}}},
      {"response", llvm::json::Object{{"status", 200}}}});
  llvm::raw_string_ostream(Text) << llvm::json::Value(llvm::json::Object{
      {"log", llvm::json::Object{{"version", "1.2"},
                                 {"entries", std::move(Entries)}}}});
  return Text;
}
} // namespace

TEST(WebPassiveReport, ObservedJSONCountsTypesWithoutValuesOrFieldNames) {
  const auto Bytes = har(llvm::json::Object{
      {"mimeType", "application/json; charset=utf-8"},
      {"text", R"({"CANARY":"CANARY_VALUE","nested":[true,null,3]})"}});
  const auto Capture = inspectHAR("a", Bytes);
  const auto Shapes = inspectPassiveHARShapes(Capture, Bytes);
  ASSERT_GT(Shapes.size(), 2u);
  const auto &R = Shapes[0];
  EXPECT_EQ(R.ShapeStatus, "observed_json_types");
  EXPECT_EQ(R.ShapeKind, "object");
  EXPECT_EQ(R.Members, 2u);
  EXPECT_EQ(R.Types, (std::map<std::string, uint64_t>{{"object", 1},
                                                      {"array", 1},
                                                      {"string", 1},
                                                      {"boolean", 1},
                                                      {"null", 1},
                                                      {"number", 1}}));
  EXPECT_EQ(Shapes[1].ShapeStatus, "body_not_available");
  const auto Report =
      composePassiveReport(nullptr, nullptr, &Capture, Shapes, nullptr);
  ASSERT_EQ(Report.Records.size(), Shapes.size() + 1);
  EXPECT_EQ(Report.StaticRecords, 0u);
  EXPECT_EQ(Report.ObservedRecords, Report.Records.size());
  EXPECT_EQ(Report.HTTPPairs, 0u);
  std::string Published;
  llvm::raw_string_ostream(Published)
      << llvm::json::Value(passive_report::record(R));
  EXPECT_EQ(Published.find("CANARY"), std::string::npos);
  EXPECT_NE(Published.find("server_constraints"), std::string::npos);
  const auto Summary = passive_report::summary(Report, 1);
  EXPECT_EQ(Summary.getString("server_constraints"), "unknown");
  EXPECT_EQ(Summary.getString("inferred_optionality"), "unknown");
}

TEST(WebPassiveReport, MalformedEncodedMissingAndOversizedBodiesStayLimited) {
  for (const auto &[Body, Expected] :
       std::vector<std::pair<llvm::json::Object, std::string>>{
           {llvm::json::Object{{"mimeType", "application/json"},
                               {"text", "{CANARY"}},
            "invalid_json_body"},
           {llvm::json::Object{{"mimeType", "application/json"},
                               {"text", "CANARY"},
                               {"encoding", "base64"}},
            "encoded_body_not_analyzed"},
           {llvm::json::Object{{"mimeType", "text/plain"}, {"text", "CANARY"}},
            "content_type_not_supported"},
           {llvm::json::Object{{"mimeType", "application/json"}},
            "body_text_missing"},
           {llvm::json::Object{
                {"mimeType", "application/json"},
                {"text", std::string(MaxPassiveShapeBytes + 1, 'a')}},
            "body_byte_budget_exceeded"},
           {llvm::json::Object{{"mimeType", "application/json"},
                               {"text", "{\"x\":1,\"x\":2}"}},
            "invalid_json_body"}}) {
    const auto Bytes = har(Body);
    const auto Capture = inspectHAR("a", Bytes);
    const auto Shapes = inspectPassiveHARShapes(Capture, Bytes);
    EXPECT_EQ(Shapes[0].ShapeStatus, Expected);
    EXPECT_TRUE(Shapes[0].Types.empty());
  }
}

TEST(WebPassiveReport, ForeignBytesSelectionsAndRecordBudgetsRefuse) {
  const auto Bytes =
      har(llvm::json::Object{{"mimeType", "application/json"}, {"text", "{}"}});
  const auto C = inspectHAR("a", Bytes);
  EXPECT_THROW(inspectPassiveHARShapes(C, Bytes + " "), Error);
  EXPECT_THROW(composePassiveReport(nullptr, nullptr, nullptr, {}, nullptr),
               Error);
  const auto Shapes = inspectPassiveHARShapes(C, Bytes);
  EXPECT_THROW(composePassiveReport(nullptr, nullptr, nullptr, Shapes, nullptr),
               Error);
  auto Excess = Shapes;
  Excess.resize(MaxPassiveReportRecords);
  EXPECT_THROW(composePassiveReport(nullptr, nullptr, &C, Excess, nullptr),
               Error);
  EXPECT_EQ(C.Observations.size(), 1u);
}

TEST(WebPassiveReport, StreamOnlyKeepsRecordedLinksAndUnknownFraming) {
  const auto C = inspectStream(
      "a",
      R"({"session":"CANARY","direction":"client_to_server","message":{"jsonrpc":"2.0","id":1,"method":"CANARY"}}
{"session":"CANARY","direction":"server_to_client","message":{"jsonrpc":"2.0","id":1,"result":{}}}
)",
      "recorded-jsonrpc-2.0-jsonl-v1");
  const auto R = composePassiveReport(nullptr, nullptr, nullptr, {}, &C);
  ASSERT_EQ(R.Records.size(), 2u);
  EXPECT_EQ(R.Records[0].RelatedID, C.Records[1].ID);
  EXPECT_EQ(R.Records[1].RelatedID, C.Records[0].ID);
  EXPECT_EQ(R.MethodCandidates, 0u);
  EXPECT_EQ(R.Records[0].Method, "method_redacted");
  EXPECT_EQ(R.SourceStatus, "not_selected");
}

TEST(WebPassiveReport, AggregateBodyWorkCannotRestartAtEveryObservation) {
  std::string Array = "[";
  for (unsigned I = 0; I < 30000; ++I)
    Array += I ? ",0" : "0";
  Array += ']';
  llvm::json::Array Entries;
  for (unsigned I = 0; I < 8; ++I)
    Entries.emplace_back(llvm::json::Object{
        {"request",
         llvm::json::Object{
             {"postData", llvm::json::Object{{"mimeType", "application/json"},
                                             {"text", Array}}}}}});
  std::string Bytes;
  llvm::raw_string_ostream(Bytes) << llvm::json::Value(llvm::json::Object{
      {"log", llvm::json::Object{{"version", "1.2"},
                                 {"entries", std::move(Entries)}}}});
  const auto Capture = inspectHAR("a", Bytes);
  EXPECT_THROW(inspectPassiveHARShapes(Capture, Bytes), Error);
  EXPECT_EQ(Capture.Observations.size(), 8u);
}
