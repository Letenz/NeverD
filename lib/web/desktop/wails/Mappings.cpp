//===- Mappings.cpp - Unambiguous stored Wails pointer ranges
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared file-backed permission and overlap policy for admitted containers.
/// Virtual tails and mutable storage never substitute for captured assets.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "neverd/web/Limits.h"

namespace neverd::web::wails_detail {
void Reader::admitMapping(Mapping M) {
  within(M.File, Input.Content.size());
  within(M.Virtual, UINT64_MAX);
  if (!(M.Flags & 4) || (M.Flags & ~7U) || (M.Flags & 3) == 3 ||
      M.File.Size > M.Virtual.Size)
    throw Error("wails_invalid_load_mapping");
  if (Mappings.size() ==
      ::neverd::limits::web::kMappingsReaderAdmitMappingsSizeLimit)
    throw Error("wails_native_table_budget_exceeded");
  for (const auto &Other : Mappings) {
    step();
    if (overlaps(M.File, Other.File) || overlaps(M.Virtual, Other.Virtual))
      throw Error("wails_ambiguous_mapping");
  }
  Mappings.push_back(M);
}

Range Reader::mapped(uint64_t Address, uint64_t Size, bool ReadOnly) {
  within({Address, Size}, UINT64_MAX);
  std::optional<Range> Out;
  for (const auto &M : Mappings) {
    step();
    const Range Backed{M.Virtual.Offset, M.File.Size};
    if (!contains(Backed, {Address, Size}) || Address == Backed.end())
      continue;
    if (Out || (ReadOnly && M.Flags != 4))
      throw Error("wails_ambiguous_mapping");
    Out = {M.File.Offset + Address - M.Virtual.Offset, Size};
  }
  if (!Out)
    throw Error("wails_non_file_backed_pointer");
  return *Out;
}
} // namespace neverd::web::wails_detail
