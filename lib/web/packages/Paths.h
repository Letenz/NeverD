//===- Paths.h - Confined package inventory paths -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One lexical path policy for package metadata and install entry candidates.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"

#include <string_view>

namespace neverd::web::package_paths {
inline bool confined(std::string_view Path, bool Empty = false) {
  if (Path.empty())
    return Empty;
  if (Path.size() > ::neverd::limits::web::kPathsConfinedPathSizeLimit ||
      Path.front() == '/' || Path.back() == '/' ||
      Path.find_first_of("\\:") != std::string_view::npos)
    return false;
  unsigned Depth = 0;
  for (unsigned char C : Path)
    if (C < 32 || C == 127)
      return false;
  while (!Path.empty()) {
    const auto End = Path.find('/');
    const auto Part = Path.substr(0, End);
    if (Part.empty() || Part == "." || Part == ".." ||
        ++Depth > ::neverd::limits::web::kPathsConfinedDepthLimit)
      return false;
    if (End == std::string_view::npos)
      break;
    Path.remove_prefix(End + 1);
  }
  return true;
}
} // namespace neverd::web::package_paths
