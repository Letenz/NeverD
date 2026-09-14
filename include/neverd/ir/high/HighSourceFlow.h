//===- HighSourceFlow.h - Source flow analysis -------------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_HIGH_HIGHSOURCEFLOW_H
#define NEVERD_IR_HIGH_HIGHSOURCEFLOW_H

#include "neverd/ir/high/HighIR.h"

#include <string>
#include <tuple>
#include <vector>

namespace neverd {
using HighSourceLocalIdentity = std::tuple<int, int, int, int64_t>;
HighSourceLocalIdentity highSourceLocalIdentity(const MedVar &Variable);
bool highSourceFrameBase(const HighFunc &Function, const MedVar &Variable);

enum class HighSourceFlowIssue {
  ControlFlow,
  DefiniteAssignment,
  MalformedExpression,
  Exception,
  Budget
};
struct HighSourceFlowDiagnostic {
  HighSourceFlowIssue Issue;
  std::string Reason;
  va_t StatementAddress = 0;
  const HighExpr *Expression = nullptr;
  va_t RelatedAddress = 0;
};
/// Diagnostics borrow expressions from Function. The function must outlive
/// this report; expression addresses are never persistent identities.
struct HighSourceFlowReport {
  bool Complete = true;
  std::vector<HighSourceFlowDiagnostic> Items;
  void add(HighSourceFlowIssue Issue, std::string Reason, va_t Address = 0,
           const HighExpr *Expression = nullptr, va_t RelatedAddress = 0);
};
HighSourceFlowReport analyzeHighSourceFlow(const HighFunc &Function,
                                           bool NeedsReturn);
/// Remove side-effect-free PHI copies whose values cannot be observed on any
/// feasible emitted path. Unknown control flow or exhausted analysis budgets
/// leave the function unchanged. Retains addresses used as source labels.
bool eliminateHighDeadPhiCopies(HighFunc &Function);
} // namespace neverd
#endif
