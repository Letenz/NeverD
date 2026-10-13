//===- Reader.h - Private pkg reader boundaries ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded locator, index and byte partition ownership for the pkg profile.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Pkg.h"

namespace neverd::web::pkg_detail {
struct Layout {
  uint64_t PreludeOffset = 0, PreludeSize = 0;
};
void step(PkgExtraction &E, uint64_t Count = 1);
void metadata(PkgExtraction &E, uint64_t Count);
Layout locate(const Artifact &Input, PkgExtraction &E);
void readIndex(const Artifact &Input, std::string_view Prelude,
               PkgExtraction &E);
void hashStores(PkgExtraction &E);
} // namespace neverd::web::pkg_detail
