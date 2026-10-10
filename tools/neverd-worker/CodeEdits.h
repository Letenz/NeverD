#pragma once
#include "Protocol.h"

#include <functional>
#include <optional>

namespace neverd::worker {
/// Analyst presentation edits, scoped to a function and source representation.
/// These never change the engine's variables, instructions or inferred types.
class CodeEdits {
public:
  static void validateRow(const Json &row);
  static Json change(Json row, const Json &request);
  /// Refuse obsolete or ambiguous targets before a durable edit is committed.
  static void validateChange(const Json &view, const Json &request);
  using Resolver =
      std::function<std::optional<std::uint64_t>(const std::string &)>;
  /// Decorate a complete source view, preserving its lines and remapping every
  /// UTF-8 span. Edit targets come from declared locals or resolved image
  /// names.
  static void decorate(Json &view, const Json &row, std::uint64_t function,
                       const std::string &representation, const Json &renames,
                       const Json &annotations, const Resolver &resolve);
};
} // namespace neverd::worker
