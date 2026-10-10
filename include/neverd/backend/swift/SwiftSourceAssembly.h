#ifndef NEVERD_BACKEND_SWIFT_SWIFTSOURCEASSEMBLY_H
#define NEVERD_BACKEND_SWIFT_SWIFTSOURCEASSEMBLY_H

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

struct SwiftSourceUnitText {
  std::string Source;
  std::string ModulePreamble;
};

/// Units are individually self-contained. Assemble their bodies in supplied
/// order, with each distinct file-level preamble once at the beginning.
/// An absent prefix is an invalid unit, never permission to discard text.
inline std::optional<std::string>
assembleSwiftSourceUnits(const std::vector<SwiftSourceUnitText> &Units) {
  std::set<std::string> Preambles;
  std::string Body;
  for (const auto &Unit : Units) {
    if (!Unit.Source.starts_with(Unit.ModulePreamble) ||
        Unit.ModulePreamble.find('\0') != std::string::npos)
      return std::nullopt;
    if (!Unit.ModulePreamble.empty())
      Preambles.insert(Unit.ModulePreamble);
    Body += Unit.Source.substr(Unit.ModulePreamble.size()) + "\n";
  }
  std::string Source;
  for (const auto &Preamble : Preambles)
    Source += Preamble;
  return Source + Body;
}

} // namespace neverd
#endif
