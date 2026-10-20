//===- SourceVariableEdits.h - Explicit source association policy -*- C++
//-*-===//
#ifndef NEVERD_IR_SOURCEVARIABLEEDITS_H
#define NEVERD_IR_SOURCEVARIABLEEDITS_H
#include <string>
#include <vector>
namespace neverd {
struct SourceVariableEdit {
  std::string Kind;
  std::string From;
  std::string To;
  std::string Snapshot;
  /// Request-local profile fingerprint, computed by the source API rather
  /// than read from the project. Never machine/debug/rewrite authority.
  std::string AfterSnapshot;
  bool operator==(const SourceVariableEdit &) const = default;
};
struct SourceVariableEditResult {
  bool Applied = false;
  std::string Refusal;
  std::vector<std::string> Outputs;
};
} // namespace neverd
#endif
