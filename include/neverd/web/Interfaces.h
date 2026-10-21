//===- Interfaces.h - Passive request and capture evidence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded source candidates, redacted HAR observations and explicit joins.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceValues.h"

#include <map>
#include <optional>

namespace neverd::web {

inline constexpr std::string_view HARProfile = "har-1.2-metadata-v1";
inline constexpr std::string_view SourceInterfaceProfile =
    "fetch-websocket-xhr-wrappers-syntax-v4";
inline constexpr std::string_view SourceRequestWrapperProfile =
    "direct-lexical-fetch-forwarding-v1";
inline constexpr std::string_view InterfaceCorrelationProfile =
    "absolute-http-method-origin-path-v1";
inline constexpr std::string_view InterfaceRedactionPolicy =
    "passive-interface-metadata-v1";

/// Only fixed classifications/counts may be serialized. Origin and Path are
/// private comparison material; they can contain personal data or secrets.
/// Query values, credentials and fragments are never retained here.
struct InterfaceEndpoint {
  std::string Status = "absent";
  std::string Scheme = "unknown";
  std::string Origin, Path;
  uint64_t PathSegments = 0, QueryParameters = 0;
  bool CredentialsPresent = false, FragmentPresent = false;

  bool comparable() const {
    return Status == "absolute_http" && !CredentialsPresent;
  }
};

InterfaceEndpoint inspectInterfaceEndpoint(std::string_view URL);
/// Returns a fixed recognized method or "other". Fetch normalizes only the
/// six methods defined by its method-normalization algorithm.
std::string interfaceMethod(std::string_view Method, bool Fetch = false);
/// Fetch and XHR forbid CONNECT, TRACE and TRACK with ASCII-insensitive
/// matching.
bool interfaceForbiddenMethod(std::string_view Method);
/// Returns a fixed allowlisted name, "sensitive", or "other"; never echoes a
/// caller-supplied field name.
std::string interfaceHeaderKind(std::string_view Name);

struct HARFields {
  bool Present = false;
  uint64_t Count = 0, Incomplete = 0;
  std::map<std::string, uint64_t> HeaderKinds;
};

struct HARObservation {
  std::string ID, RequestID, ResponseID;
  std::string TimestampStatus = "absent", Timestamp;
  std::string Method = "unknown", MethodStatus = "absent";
  std::optional<int64_t> Status;
  bool RequestPresent = false, ResponsePresent = false;
  InterfaceEndpoint Endpoint;
  HARFields RequestHeaders, ResponseHeaders, RequestCookies, ResponseCookies;
  HARFields Query, PostParameters;
  bool PostDataPresent = false, RequestTextPresent = false;
  bool ResponseContentPresent = false, ResponseTextPresent = false;
  std::string ResponseEncoding = "absent";
  bool WebSocketExtensionPresent = false;
};

struct HARCapture {
  std::string ID, ArtifactID, BlobHash;
  std::vector<HARObservation> Observations;
  uint64_t Steps = 0, Fields = 0, PrivateBytes = 0;
  uint64_t ExtensionFields = 0, MissingRequests = 0, MissingResponses = 0;
  uint64_t MissingTimestamps = 0, InvalidTimestamps = 0;
  bool CreatorPresent = false;
};

/// Input is captured locally; callers must preview and commit the redaction
/// policy before publishing observations. Bodies/extensions are never decoded.
HARCapture inspectHAR(std::string_view ArtifactID, std::string_view Bytes);

struct SourceInterface {
  std::string ID, Kind;
  uint32_t Node = NoSourceIndex, URLNode = NoSourceIndex;
  uint32_t OptionsNode = NoSourceIndex, MethodNode = NoSourceIndex;
  uint32_t HeadersNode = NoSourceIndex, BodyNode = NoSourceIndex;
  uint32_t ConstructorNode = NoSourceIndex, ReceiverBinding = NoSourceIndex;
  uint32_t ReceiverNode = NoSourceIndex;
  uint32_t AsyncNode = NoSourceIndex, HeaderNameNode = NoSourceIndex;
  uint32_t HeaderValueNode = NoSourceIndex, HandlerNode = NoSourceIndex;
  uint32_t WrapperNode = NoSourceIndex, WrapperRequestNode = NoSourceIndex;
  uint32_t WrapperBinding = NoSourceIndex;
  uint32_t URLParameterNode = NoSourceIndex,
           OptionsParameterNode = NoSourceIndex;
  uint32_t URLArgumentIndex = NoSourceIndex,
           OptionsArgumentIndex = NoSourceIndex;
  std::string Method = "unknown", MethodStatus = "unknown";
  std::string URLStatus = "absent", OptionsStatus = "absent";
  std::string ReceiverStatus = "not_applicable";
  std::string ArgumentsStatus = "not_applicable";
  std::string CredentialArgumentsStatus = "not_applicable";
  std::string BodyStatus = "not_applicable", HeaderKind = "not_applicable";
  std::string EventKind = "not_applicable", HandlerStatus = "not_applicable";
  std::string WrapperStatus = "not_applicable";
  uint64_t TemplateHoles = 0;
  bool Optional = false;
  InterfaceEndpoint Endpoint;
};

struct SourceInterfaces {
  std::string ID, SourceID, BindingID, ValueID;
  std::string Status = "unavailable", Reason;
  std::vector<SourceInterface> Records;
  uint64_t Steps = 0, PrivateBytes = 0, ExcludedDispatches = 0;
  uint64_t WrapperDefinitions = 0;
};

SourceInterfaces analyzeSourceInterfaces(const SourceAnalysis &Source,
                                         const SourceBindingAnalysis &Bindings,
                                         const SourceValueAnalysis &Values);

struct InterfacePair {
  std::string ID;
  uint32_t Source = 0, Observation = 0;
};

struct InterfaceCorrelation {
  std::string ID, SourceAnalysisID, CaptureID;
  std::vector<InterfacePair> Pairs;
  uint64_t EligibleSources = 0, EligibleObservations = 0, Steps = 0;
};

InterfaceCorrelation correlateInterfaces(const SourceInterfaces &Source,
                                         const HARCapture &Capture);

} // namespace neverd::web
