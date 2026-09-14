//===- SourceProjectionFlow.h - Source flow adapter -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_SDK_CAPI_SOURCEPROJECTIONFLOW_H
#define NEVERD_SDK_CAPI_SOURCEPROJECTIONFLOW_H

#include "SourceProjectionDiagnostics.h"

#include "neverd/ir/high/HighSourceFlow.h"

namespace neverd::sdk::objc_projection_detail {
using LocalIdentity = HighSourceLocalIdentity;
inline LocalIdentity localIdentity(const MedVar &Variable) {
  return highSourceLocalIdentity(Variable);
}
inline bool sourceFrameBase(const HighFunc &Function, const MedVar &Variable) {
  return highSourceFrameBase(Function, Variable);
}
class SourceProjectionFlow {
  const HighFunc &Function;
  SourceProjectionDiagnostics &Diagnostics;

public:
  SourceProjectionFlow(const HighFunc &Function,
                       SourceProjectionDiagnostics &Diagnostics)
      : Function(Function), Diagnostics(Diagnostics) {}
  void collect(bool NeedsReturn) {
    auto Report = analyzeHighSourceFlow(Function, NeedsReturn);
    Diagnostics.Complete &= Report.Complete;
    for (const auto &Item : Report.Items) {
      SourceProjectionIssue Issue;
      switch (Item.Issue) {
      case HighSourceFlowIssue::ControlFlow:
        Issue = SourceProjectionIssue::ControlFlow;
        break;
      case HighSourceFlowIssue::DefiniteAssignment:
        Issue = SourceProjectionIssue::DefiniteAssignment;
        break;
      case HighSourceFlowIssue::MalformedExpression:
        Issue = SourceProjectionIssue::MalformedExpression;
        break;
      case HighSourceFlowIssue::Exception:
        Issue = SourceProjectionIssue::Exception;
        break;
      case HighSourceFlowIssue::Budget:
        Issue = SourceProjectionIssue::Budget;
        break;
      }
      Diagnostics.add(Issue, Item.Reason, Item.StatementAddress,
                      Item.Expression, Item.RelatedAddress);
    }
  }
};
} // namespace neverd::sdk::objc_projection_detail
#endif
