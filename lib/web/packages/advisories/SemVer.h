//===- SemVer.h - Bounded semantic version precedence --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Strict SemVer 2.0.0 metadata comparison without npm range resolution.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::web::package_advisories {

struct SemVer {
  std::array<std::string, 3> Core;
  std::vector<std::string> Prerelease;
};

/// No prefixes, whitespace, abbreviated versions or numeric overflow coercion.
/// Build components are validated but do not participate in precedence.
std::optional<SemVer> parseSemVer(std::string_view Text);
/// Return -1, 0 or 1 using decimal digit strings, never bounded integer casts.
int compareSemVer(const SemVer &Before, const SemVer &After);

} // namespace neverd::web::package_advisories

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::web::package_advisories {
inline constexpr size_t MaxSemVerBytes =
    ::neverd::limits::web::kSemVerMaxSemVerBytes;
}
