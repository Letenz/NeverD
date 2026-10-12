//===- ELF.cpp - File-backed Wails ELF layout ----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualified ELF64 little-endian ET_EXEC mappings. Other containers and
/// architectures require separately measured producer profiles.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include <algorithm>

namespace neverd::web::wails_detail {
void admitELF(Reader &R) {
  const auto H = R.read({0, 64});
  if (H.compare(0, 7, "\177ELF\2\1\1", 7) || (H[7] != 0 && H[7] != 3) ||
      number(H, 16, 2) != 2 || number(H, 18, 2) != 62 ||
      number(H, 20, 4) != 1 || number(H, 52, 2) != 64 || number(H, 54, 2) != 56)
    throw Error("wails_unsupported_container");
  const auto Count = number(H, 56, 2);
  if (!Count || Count > 1024)
    throw Error("wails_native_table_budget_exceeded");
  const Range PH{number(H, 32, 8), Count * 56};
  if (PH.Offset < 64)
    throw Error("wails_overlapping_native_metadata");
  const auto Headers = R.read(PH);
  R.PrivateRanges = {{0, 64}, PH};
  for (uint64_t I = 0; I < Count; ++I) {
    R.step();
    const auto At = I * 56;
    if (number(Headers, At, 4) != 1)
      continue;
    Mapping M{{number(Headers, At + 8, 8), number(Headers, At + 32, 8)},
              {number(Headers, At + 16, 8), number(Headers, At + 40, 8)},
              uint32_t(number(Headers, At + 4, 4))};
    within(M.File, R.Input.Content.size());
    within(M.Virtual, UINT64_MAX);
    const auto Alignment = number(Headers, At + 48, 8);
    if (!(M.Flags & 4) || (M.Flags & ~7U) || (M.Flags & 3) == 3 ||
        M.File.Size > M.Virtual.Size ||
        (Alignment > 1 &&
         ((Alignment & (Alignment - 1)) ||
          M.File.Offset % Alignment != M.Virtual.Offset % Alignment)))
      throw Error("wails_invalid_load_mapping");
    if (R.Mappings.size() == 128)
      throw Error("wails_native_table_budget_exceeded");
    for (const auto &Other : R.Mappings) {
      R.step();
      if (overlaps(M.File, Other.File) || overlaps(M.Virtual, Other.Virtual))
        throw Error("wails_ambiguous_mapping");
    }
    R.Mappings.push_back(M);
  }
  if (R.Mappings.empty())
    throw Error("wails_invalid_load_mapping");
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
