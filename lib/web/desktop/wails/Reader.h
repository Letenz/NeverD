//===- Reader.h - Bounded Wails layout admission ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private physical ranges, work accounting and Go embed.FS format ownership.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Error.h"
#include "neverd/web/Wails.h"

#include <optional>

namespace neverd::web::wails_detail {
struct Range {
  uint64_t Offset = 0, Size = 0;
  uint64_t end() const { return Offset + Size; }
};
inline void within(Range R, uint64_t Size) {
  if (R.Offset > Size || R.Size > Size - R.Offset)
    throw Error("wails_range_out_of_bounds");
}
inline bool contains(Range A, Range B) {
  return B.Offset >= A.Offset && B.Offset - A.Offset <= A.Size &&
         B.Size <= A.Size - (B.Offset - A.Offset);
}
inline bool overlaps(Range A, Range B) {
  return A.Size && B.Size && A.Offset < B.end() && B.Offset < A.end();
}
uint64_t number(std::string_view Bytes, uint64_t At, unsigned Width);
struct Mapping {
  Range File, Virtual;
  uint32_t Flags = 0;
};
struct Reader {
  const Artifact &Input;
  WailsExtraction &Evidence;
  WailsLimits Limits;
  std::vector<Mapping> Mappings;
  std::vector<Range> PrivateRanges;
  std::vector<std::string> ExpectedHashes;

  void step(uint64_t Count = 1);
  void metadata(uint64_t Count);
  std::string read(Range R);
  Range mapped(uint64_t Address, uint64_t Size, bool ReadOnly);
};

void admitELF(Reader &R);
void admitBuildInfo(Reader &R);
void readTables(Reader &R);
void verifyContents(Reader &R);
} // namespace neverd::web::wails_detail
