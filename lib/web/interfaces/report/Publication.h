//===- Publication.h - Passive report metadata publication --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed JSON projections shared by selected reports and pages.
///
//===----------------------------------------------------------------------===//
#pragma once
#include "neverd/web/PassiveReport.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::passive_report {
llvm::json::Object summary(const PassiveReport &Report, uint64_t Revision);
llvm::json::Object record(const PassiveReportRecord &Record);
} // namespace neverd::web::passive_report
