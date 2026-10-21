//===- NativeInput.h - Immutable native handoff ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable native handoff.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Blob.h"
#include "neverd/web/Limits.h"

#include <string>
#include <string_view>

namespace neverd::web {
inline constexpr std::string_view NativeHandoffProfile =
    "immutable-native-handoff-v1";

/// A selected immutable occurrence, retaining its spool independently of the
/// originating web session. Provenance contains only fixed metadata and IDs.
struct NativeInput {
  Blob Content;
  std::string BlobHash, Provenance;
};
} // namespace neverd::web
