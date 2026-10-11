//===- Identity.h - Literal npm advisory identity agreement --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Admit a versionless npm PURL subset; no resolver or URL normalization.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <string_view>

namespace neverd::web::package_advisories {
inline std::string identityStatus(std::string_view Ecosystem,
                                  std::string_view Name,
                                  std::string_view PURL) {
  if (Ecosystem.empty() || Name.empty())
    return "package_identity_incomplete";
  if (Ecosystem != "npm")
    return "other_declared_ecosystem";
  if (PURL.empty())
    return "name_and_ecosystem_declared";
  const auto Component = [](std::string_view Text) {
    return !Text.empty() && Text != "." && Text != ".." &&
           Text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQR"
                                  "STUVWXYZ0123456789._~-") == Text.npos;
  };
  std::string Expected = "pkg:npm/";
  if (Name.starts_with('@')) {
    const auto Slash = Name.find('/');
    if (Slash == Name.npos || !Component(Name.substr(1, Slash - 1)) ||
        !Component(Name.substr(Slash + 1)))
      return "unsupported_or_conflicting_purl";
    Expected += "%40";
    Expected += Name.substr(1);
  } else {
    if (!Component(Name))
      return "unsupported_or_conflicting_purl";
    Expected += Name;
  }
  return PURL == Expected ? "matching_versionless_npm_purl"
                          : "unsupported_or_conflicting_purl";
}
} // namespace neverd::web::package_advisories
