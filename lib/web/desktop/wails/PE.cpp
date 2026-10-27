//===- PE.cpp - Stored Wails PE image mappings ----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Qualified PE32+ x64 section mapping at the stored preferred image base.
/// No loader relocation, signature verification or target execution occurs.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web::wails_detail {
namespace {
bool powerOfTwo(uint64_t N) { return N && !(N & (N - 1)); }
} // namespace

void admitPE(Reader &R) {
  const auto DOS = R.read({0, 64});
  const auto PE = number(DOS, 60, 4);
  if (!DOS.starts_with("MZ") || PE < 64 || PE % 8)
    throw Error("wails_unsupported_container");
  const auto H = R.read({PE, 24});
  const auto Flags = number(H, 22, 2);
  if (H.compare(0, 4, "PE\0\0", 4) || number(H, 4, 2) != 0x8664 ||
      !(Flags & 2) || (Flags & 0x2100) || number(H, 20, 2) != 240)
    throw Error("wails_unsupported_container");
  const auto Count = number(H, 6, 2);
  if (!Count || Count > ::neverd::limits::web::kPEAdmitPECountLimit)
    throw Error("wails_native_table_budget_exceeded");
  const auto O = R.read({PE + 24, 240});
  if (number(O, 0, 2) != 0x20b || number(O, 108, 4) != 16)
    throw Error("wails_unsupported_container");
  const auto Base = number(O, 24, 8);
  const auto SectionAlignment = number(O, 32, 4);
  const auto FileAlignment = number(O, 36, 4);
  const auto ImageSize = number(O, 56, 4);
  const auto HeaderSize = number(O, 60, 4);
  const Range Sections{PE + 264, Count * 40};
  if (!Base || Base % 65536 || !powerOfTwo(FileAlignment) ||
      FileAlignment < 512 || FileAlignment > 65536 ||
      !powerOfTwo(SectionAlignment) || SectionAlignment < 4096 ||
      SectionAlignment < FileAlignment || !ImageSize ||
      ImageSize % SectionAlignment || HeaderSize % FileAlignment ||
      Sections.end() > HeaderSize || HeaderSize > ImageSize)
    throw Error("wails_invalid_load_mapping");
  within({Base, ImageSize}, UINT64_MAX);
  within({0, HeaderSize}, R.Input.Content.size());
  const auto Headers = R.read(Sections);
  R.PrivateRanges = {{0, HeaderSize}};
  std::vector<Range> RawRanges;
  for (uint64_t I = 0; I < Count; ++I) {
    R.step();
    const auto At = I * 40;
    const auto VirtualSize = number(Headers, At + 8, 4);
    const auto RVA = number(Headers, At + 12, 4);
    const Range Raw{number(Headers, At + 20, 4), number(Headers, At + 16, 4)};
    const auto Characteristics = number(Headers, At + 36, 4);
    const auto Access = uint32_t((Characteristics >> 29) & 7);
    // PE's high bits are execute/read/write, while the shared mapping owner
    // uses execute/write/read. Preserve permissions rather than section names.
    const auto Permissions =
        (Access & 1) | ((Access & 2) << 1) | ((Access & 4) >> 1);
    if (!VirtualSize || RVA < HeaderSize || RVA % SectionAlignment ||
        RVA > ImageSize || VirtualSize > ImageSize - RVA ||
        (Raw.Size && (Raw.Offset < HeaderSize || Raw.Offset % FileAlignment ||
                      Raw.Size % FileAlignment)) ||
        (Characteristics & 0x10000000))
      throw Error("wails_invalid_load_mapping");
    within(Raw, R.Input.Content.size());
    for (const auto Other : RawRanges) {
      R.step();
      if (overlaps(Raw, Other))
        throw Error("wails_ambiguous_mapping");
    }
    RawRanges.push_back(Raw);
    // File-alignment padding and zero-filled virtual tails never satisfy a
    // stored pointer. Only the intersection has captured physical bytes.
    R.admitMapping({{Raw.Offset, std::min(Raw.Size, VirtualSize)},
                    {Base + RVA, VirtualSize},
                    Permissions});
  }
}
} // namespace neverd::web::wails_detail
