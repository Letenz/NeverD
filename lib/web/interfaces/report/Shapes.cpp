//===- Shapes.cpp - Redacted observed HAR JSON type summaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded observed types without publishing keys, values or server schemas.
///
//===----------------------------------------------------------------------===//

#include "../../JsonReader.h"

#include "neverd/web/Error.h"
#include "neverd/web/PassiveReport.h"

namespace neverd::web {
namespace {
std::string kind(const llvm::json::Value &V) {
  if (V.getAsObject())
    return "object";
  if (V.getAsArray())
    return "array";
  if (V.getAsString())
    return "string";
  if (V.getAsBoolean())
    return "boolean";
  if (V.getAsNumber())
    return "number";
  return "null";
}
bool jsonMIME(llvm::StringRef MIME) {
  const auto End = MIME.find(';');
  MIME = MIME.take_front(End).trim();
  return MIME.equals_insensitive("application/json") ||
         (MIME.starts_with_insensitive("application/") &&
          MIME.ends_with_insensitive("+json"));
}
void countTypes(const llvm::json::Value &V, PassiveReportRecord &R) {
  ++R.Types[kind(V)];
  if (const auto *O = V.getAsObject())
    for (const auto &KV : *O)
      countTypes(KV.second, R);
  else if (const auto *A = V.getAsArray())
    for (const auto &Item : *A)
      countTypes(Item, R);
}

PassiveReportRecord body(const HARCapture &C, const HARObservation &O,
                         const llvm::json::Object *Body, bool Response,
                         uint64_t &Nodes) {
  PassiveReportRecord R;
  R.Kind = Response ? "observed_response_shape" : "observed_request_shape";
  R.EvidenceClass = "imported_observation";
  R.AnchorID = Response ? O.ResponseID : O.RequestID;
  R.ParentID = O.ID;
  R.ArtifactID = C.ArtifactID;
  R.ShapeStatus = "body_not_available";
  if (!Body)
    return R;
  const auto *Encoding = Body->get("encoding");
  if (Encoding &&
      (!Encoding->getAsString() || !Encoding->getAsString()->empty())) {
    R.ShapeStatus = "encoded_body_not_analyzed";
    return R;
  }
  const auto MIME = Body->getString("mimeType");
  if (!MIME || !jsonMIME(*MIME)) {
    R.ShapeStatus = "content_type_not_supported";
    return R;
  }
  const auto Text = Body->getString("text");
  if (!Text) {
    R.ShapeStatus = "body_text_missing";
    return R;
  }
  if (Text->size() > MaxPassiveShapeBytes) {
    R.ShapeStatus = "body_byte_budget_exceeded";
    return R;
  }
  uint64_t Used = 0;
  try {
    const auto V =
        parseBoundedJSON(*Text,
                         {MaxPassiveShapeBytes, 16,
                          MaxPassiveShapeNodes - Nodes, MaxPassiveShapeBytes},
                         &Used);
    Nodes += Used;
    R.ShapeStatus = "observed_json_types";
    R.ShapeKind = kind(V);
    if (const auto *Object = V.getAsObject())
      R.Members = Object->size();
    else if (const auto *Array = V.getAsArray())
      R.Members = Array->size();
    countTypes(V, R);
  } catch (const Error &E) {
    Nodes += Used;
    const auto Code = std::string_view(E.what());
    if (Code == "json_node_budget_exceeded")
      throw Error("passive_shape_aggregate_budget_exceeded");
    R.ShapeStatus = Code.find("budget_exceeded") != std::string_view::npos
                        ? "body_shape_budget_exceeded"
                        : "invalid_json_body";
  }
  return R;
}
} // namespace

std::vector<PassiveReportRecord>
inspectPassiveHARShapes(const HARCapture &C, std::string_view Bytes) {
  if (sha256(Bytes) != C.BlobHash)
    throw Error("passive_report_capture_hash_mismatch");
  if (Bytes.starts_with("\xef\xbb\xbf"))
    Bytes.remove_prefix(3);
  const auto V =
      parseBoundedJSON(Bytes, {MaxHARBytes, 32, 200000, MaxHARBytes});
  const auto *Root = V.getAsObject();
  const auto *Log = Root ? Root->getObject("log") : nullptr;
  const auto *Entries = Log ? Log->getArray("entries") : nullptr;
  if (!Entries || Entries->size() != C.Observations.size())
    throw Error("passive_report_capture_model_mismatch");
  std::vector<PassiveReportRecord> Result;
  uint64_t Nodes = 0;
  for (uint32_t I = 0; I < Entries->size(); ++I) {
    const auto *Entry = (*Entries)[I].getAsObject();
    const auto *Request = Entry ? Entry->getObject("request") : nullptr;
    const auto *Response = Entry ? Entry->getObject("response") : nullptr;
    Result.push_back(body(C, C.Observations[I],
                          Request ? Request->getObject("postData") : nullptr,
                          false, Nodes));
    Result.push_back(body(C, C.Observations[I],
                          Response ? Response->getObject("content") : nullptr,
                          true, Nodes));
  }
  return Result;
}
} // namespace neverd::web
