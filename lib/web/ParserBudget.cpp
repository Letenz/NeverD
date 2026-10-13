//===- ParserBudget.cpp - JavaScript parser resource limits ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript parser resource limits.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/ParserBudget.h"

#include "neverd/web/Error.h"
#include "neverd/web/SourceLimits.h"

namespace neverd::web {
namespace {
thread_local ParserBudget *Active = nullptr;
}

ParserBudget::ParserBudget(SourceAnalysisMode Mode) : Previous(Active) {
  const auto Limits = sourceLimits(Mode);
  MaxBytes = Limits.ParserBytes;
  MaxWork = Limits.ParserAllocations;
  Deadline = std::chrono::steady_clock::now() +
             std::chrono::seconds(Limits.ParserSeconds);
  Active = this;
}

ParserBudget::~ParserBudget() { Active = Previous; }

void ParserBudget::charge(size_t Count) {
  if (Count > MaxBytes - Bytes || ++Work > MaxWork)
    throw Error("parser_budget_exceeded");
  Bytes += Count;
  if ((Work & 63) == 0 && std::chrono::steady_clock::now() > Deadline)
    throw Error("parser_deadline_exceeded");
}

void chargeParserAllocation(size_t Bytes) {
  if (!Active)
    throw Error("parser_budget_missing");
  Active->charge(Bytes);
}
} // namespace neverd::web
