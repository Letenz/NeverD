//===- SourceInterfaces.cpp - Passive source evidence coordination -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source-order syntax evidence with shared admission and atomic failure.
///
//===----------------------------------------------------------------------===//

#include "SourceContext.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

namespace neverd::web {

SourceInterfaces analyzeSourceInterfaces(const SourceAnalysis &Source,
                                         const SourceBindingAnalysis &Bindings,
                                         const SourceValueAnalysis &Values) {
  interfaces::SourceContext C{Source, Bindings, Values};
  C.A.SourceID = Source.ID;
  C.A.BindingID = Bindings.ID;
  C.A.ValueID = Values.ID;
  C.A.ID = identity("source-interfaces", {Source.ID, Bindings.ID, Values.ID,
                                          SourceInterfaceProfile});
  try {
    C.prepare();
    const auto Constructors = interfaces::xhrConstructors(C);
    const auto Wrappers = interfaces::requestWrappers(C);
    for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
      C.step();
      interfaces::inspectRequestSyntax(C, I);
      interfaces::inspectRequestWrapper(C, Wrappers, I);
      interfaces::inspectXHR(C, Constructors, I);
    }
    C.A.Status = "partial";
    C.A.Reason = "source_callee_and_receiver_not_runtime_identity";
  } catch (const Error &E) {
    C.A.Records.clear();
    C.A.PrivateBytes = 0;
    C.A.WrapperDefinitions = 0;
    C.A.Reason = E.what();
    C.A.Status = C.A.Reason.find("budget_exceeded") != std::string::npos
                     ? "budget_exceeded"
                     : "unavailable";
  }
  return std::move(C.A);
}

} // namespace neverd::web
