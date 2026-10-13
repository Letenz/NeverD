//===- SourceRelations.cpp - Request syntax relationship coordination --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent relation budgets and atomic failure preserve request evidence.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RelationContext.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

namespace neverd::web {

InterfaceSourceRelations analyzeInterfaceSourceRelations(
    const SourceAnalysis &S, const SourceBindingAnalysis &B,
    const SourceInterfaces &I, const SourceNavigation &N) {
  return analyzeInterfaceSourceRelations(
      S, B, I, N,
      {MaxInterfaceSourceRelations,
       sourceWorkLimit(S, MaxInterfaceRelationSteps)});
}

InterfaceSourceRelations analyzeInterfaceSourceRelations(
    const SourceAnalysis &S, const SourceBindingAnalysis &B,
    const SourceInterfaces &I, const SourceNavigation &N,
    InterfaceRelationLimits Limits) {
  interface_relations::Context C{S, B, I, N, Limits};
  C.A.SourceID = S.ID;
  C.A.BindingID = B.ID;
  C.A.InterfaceID = I.ID;
  C.A.NavigationID = N.ID;
  C.A.ID = identity("interface-source-relations",
                    {I.ID, N.ID, InterfaceSourceRelationProfile});
  try {
    C.prepare();
    for (uint32_t R = 0; R < I.Records.size(); ++R) {
      C.step();
      interface_relations::requestLinks(C, R);
    }
    for (uint32_t Node = 0; Node < S.Nodes.size(); ++Node) {
      C.step();
      interface_relations::resultLinks(C, Node);
      interface_relations::transformationLinks(C, Node);
    }
    C.A.Status = "partial";
    C.A.Reason = "syntax_relationships_not_runtime_response_flow";
  } catch (const Error &E) {
    C.A.Records.clear();
    C.A.Reason = E.what();
    C.A.Status = C.A.Reason.find("budget_exceeded") != std::string::npos
                     ? "budget_exceeded"
                     : "unavailable";
  }
  return std::move(C.A);
}

} // namespace neverd::web
