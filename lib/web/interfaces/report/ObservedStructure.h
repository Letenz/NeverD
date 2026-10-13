//===- ObservedStructure.h - Private observed field dictionaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded example trees and capture-local structural aggregates.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PassiveReport.h"

#include "llvm/Support/JSON.h"

#include <tuple>

namespace neverd::web::passive_report {
std::string jsonKind(const llvm::json::Value &Value);

class ObservedStructure {
  struct SchemaNode {
    PassiveReportRecord Record;
    std::map<std::string, uint32_t, std::less<>> Fields;
    std::optional<uint32_t> Element, Parent;
  };
  using GroupKey = std::tuple<std::string, std::string, std::string, bool>;
  const HARCapture &Capture;
  std::vector<PassiveReportRecord> &Records;
  std::vector<SchemaNode> Schema;
  std::map<GroupKey, uint32_t> Groups;
  uint64_t PrivateBytes = 0;

  void chargePrivate(uint64_t Bytes);
  uint32_t schemaNode(const PassiveReportRecord &Base,
                      std::optional<uint32_t> Parent, std::string Edge,
                      uint64_t Ordinal);
  void value(const llvm::json::Value &Value, PassiveReportRecord &Summary,
             uint32_t SchemaIndex, std::string Parent, uint64_t Ordinal);

public:
  ObservedStructure(const HARCapture &C,
                    std::vector<PassiveReportRecord> &Output)
      : Capture(C), Records(Output) {}
  void inspect(const llvm::json::Value &Value,
               const HARObservation &Observation, bool Response,
               PassiveReportRecord &Summary);
  void finish();
};
} // namespace neverd::web::passive_report
