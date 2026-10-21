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

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web::wails_detail {
void admitELF(Reader &R) {
  const auto H = R.read({0, 64});
  if (H.compare(0, 7, "\177ELF\2\1\1", 7) || (H[7] != 0 && H[7] != 3) ||
      number(H, 16, 2) != 2 || number(H, 18, 2) != 62 ||
      number(H, 20, 4) != 1 || number(H, 52, 2) != 64 || number(H, 54, 2) != 56)
    throw Error("wails_unsupported_container");
  const auto Count = number(H, 56, 2);
  if (!Count || Count > ::neverd::limits::web::kELFAdmitELFCountLimit)
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
    const auto Alignment = number(Headers, At + 48, 8);
    if (Alignment > 1 &&
        ((Alignment & (Alignment - 1)) ||
         M.File.Offset % Alignment != M.Virtual.Offset % Alignment))
      throw Error("wails_invalid_load_mapping");
    R.admitMapping(M);
  }
  if (R.Mappings.empty())
    throw Error("wails_invalid_load_mapping");
}

} // namespace neverd::web::wails_detail
