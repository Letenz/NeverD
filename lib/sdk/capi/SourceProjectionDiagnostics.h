//===- SourceProjectionDiagnostics.h - Source projection evidence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_SOURCEPROJECTIONDIAGNOSTICS_H
#define NEVERD_SDK_CAPI_SOURCEPROJECTIONDIAGNOSTICS_H

#include "neverd/ir/high/HighIR.h"

#include <string>
#include <vector>

namespace neverd::sdk {

enum class SourceProjectionIssue {
  Signature,
  ABI,
  Audit,
  Body,
  Exception,
  ControlFlow,
  DefiniteAssignment,
  UnresolvedValue,
  CallBinding,
  ParameterBinding,
  IncomingValue,
  LocalDefinition,
  MalformedExpression,
  Budget,
  DataBinding,
  Dependency
};

struct SourceProjectionDiagnostic {
  SourceProjectionIssue Issue;
  std::string Reason;
  va_t StatementAddress = 0;
  // Expression evidence borrows from the checked HighFunc. Its owner must
  // outlive the report. It is never used as a serialized identity.
  const HighExpr *Expression = nullptr;
  va_t RelatedAddress = 0;
};

/// Both the compatibility gate and the evidence inventory execute the same
/// checks. FirstFailure retains their historical order and early exit; All
/// continues only checks whose prerequisites remain available.
struct SourceProjectionDiagnostics {
  enum class Mode { FirstFailure, All };
  struct Stop {};
  static constexpr size_t MaxDiagnostics = 4096;

  Mode Collection = Mode::All;
  bool Complete = true;
  std::vector<SourceProjectionDiagnostic> Items;

  explicit SourceProjectionDiagnostics(Mode Collection = Mode::All)
      : Collection(Collection) {}

  void add(SourceProjectionIssue Issue, std::string Reason,
           va_t StatementAddress = 0, const HighExpr *Expression = nullptr,
           va_t RelatedAddress = 0) {
    if (Items.size() < MaxDiagnostics) {
      Items.push_back({Issue, std::move(Reason), StatementAddress, Expression,
                       RelatedAddress});
    } else if (Items.size() == MaxDiagnostics) {
      Items.push_back(
          {SourceProjectionIssue::Budget,
           "source projection evidence exceeds its diagnostic limit"});
      Complete = false;
    }
    if (Collection == Mode::FirstFailure) {
      Complete = false;
      throw Stop{};
    }
  }

  std::string limitation() const {
    return Items.empty() ? std::string{} : Items.front().Reason;
  }
};

} // namespace neverd::sdk

#endif
