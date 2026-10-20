#pragma once
#include <string>
#include <vector>
namespace neverd {
/// Analyst-owned, source-only interpretation of one physical call occurrence.
struct SourceCallEdit {
  std::string Id;
  std::string Snapshot;
  std::string Declaration;
  std::vector<std::string> VariadicTypes;
  bool operator==(const SourceCallEdit &) const = default;
};
struct SourceCallEditResult {
  std::string Id;
  bool Applied = false;
  std::string Refusal;
};
} // namespace neverd
