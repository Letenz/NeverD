#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace neverd::worker {

/// A kind of direct reference (ReferenceKinds.def).
enum class RefKind : std::uint8_t {
#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code) Id,
#include "ReferenceKinds.def"
};

inline std::optional<RefKind> parseRefKind(std::string_view name) {
#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code)                          \
  if (name == Name)                                                            \
    return RefKind::Id;
#include "ReferenceKinds.def"
  return std::nullopt;
}

inline std::string_view refKindName(RefKind kind) {
  switch (kind) {
#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code)                          \
  case RefKind::Id:                                                            \
    return Name;
#include "ReferenceKinds.def"
  }
  return {};
}

/// The classic one-letter type: p(rocedure), j(ump), r(ead), w(rite) or
/// o(ffset).
inline char refKindLetter(RefKind kind) {
  switch (kind) {
#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code)                          \
  case RefKind::Id:                                                            \
    return Letter;
#include "ReferenceKinds.def"
  }
  return 'o';
}

/// Whether the reference transfers control rather than reaching data.
inline bool isCodeRef(RefKind kind) {
  switch (kind) {
#define NEVERD_REFERENCE_KIND(Id, Name, Letter, Code)                          \
  case RefKind::Id:                                                            \
    return Code;
#include "ReferenceKinds.def"
  }
  return false;
}

} // namespace neverd::worker
