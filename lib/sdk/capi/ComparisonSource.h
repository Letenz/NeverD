//===- ComparisonSource.h - Snapshot-bound source pages -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapts the ordinary source emitter to an exclusively owned comparison input.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_COMPARISONSOURCE_H
#define NEVERD_SDK_CAPI_COMPARISONSOURCE_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include "llvm/Support/JSON.h"

#include <cstdint>
#include <string>

namespace neverd::sdk {
struct Session;

struct ComparisonSourceOptions {
  std::string Representation = "c";
  uint64_t Offset = 0, Limit = limits::diff::kDefaultPageSize;
  /// Zero removes the additional native-instruction admission allowance.
  uint64_t MaxNativeInstructions =
      limits::diff::kDefaultSourceNativeInstructions;
  bool Refresh = false;
};

struct ComparisonSourcePage {
  diff::ComparisonState State = diff::ComparisonState::Complete;
  std::string Availability = "unavailable", Reason;
  llvm::json::Object Document;
};

ComparisonSourcePage
readComparisonSource(Session &Session, const diff::Program &Program,
                     uint32_t Index, const ComparisonSourceOptions &Options,
                     const diff::ComparisonControl &Control);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_COMPARISONSOURCE_H
