//===- COFFRegistrationEHAlignedEntry.cpp - PE32 aligned entries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/ReadOnlyBytes.h"

#include <algorithm>

namespace neverd::coff_loader::registration_detail {
namespace {

bool matchesEntry(const BinaryImage &Image, const InstallSite &Site, va_t Entry,
                  unsigned AlignSize) {
  const unsigned Prefix = 10 + AlignSize;
  const auto *Code = Image.readVA(Entry, Prefix + 7);
  if (!Code || Code[0] != 0x55 ||
      !((Code[1] == 0x8b && Code[2] == 0xec) ||
        (Code[1] == 0x89 && Code[2] == 0xe5)) ||
      Code[3] != (AlignSize == 3 ? 0x83 : 0x81) || Code[4] != 0xe4 ||
      Code[3 + AlignSize] != 0x6a || Code[4 + AlignSize] != 0xff ||
      Code[5 + AlignSize] != 0x68 ||
      readLE<uint32_t>(Code + 6 + AlignSize) != Site.HandlerVA ||
      Code[Prefix] != 0x64 || Code[Prefix + 1] != 0xa1 ||
      readLE<uint32_t>(Code + Prefix + 2) || Code[Prefix + 6] != 0x50)
    return false;
  const uint32_t Mask =
      AlignSize == 3 ? uint32_t(int8_t(Code[5])) : readLE<uint32_t>(Code + 5);
  const uint32_t Alignment = uint32_t(0) - Mask;
  if (Alignment < 4 || Alignment > 128 || (Alignment & (Alignment - 1)))
    return false;
  unsigned Cursor = Prefix + 7;
  Code = Image.readVA(Entry, Cursor + 3);
  if (!Code || Code[Cursor + 1] != 0xec ||
      (Code[Cursor] != 0x83 && Code[Cursor] != 0x81))
    return false;
  int64_t Allocation = int8_t(Code[Cursor + 2]);
  if (Code[Cursor] == 0x81) {
    Code = Image.readVA(Entry, Cursor + 6);
    if (!Code)
      return false;
    Allocation = readLE<int32_t>(Code + Cursor + 2);
    Cursor += 6;
  } else
    Cursor += 3;
  if (Allocation < 0 || Allocation > limits::kMaxSourceCallBorrowedBytes)
    return false;
  Code = Image.readVA(Entry, Cursor + 15);
  const uint8_t Saves[] = {0x53, 0x56, 0x57, 0xa1};
  if (!Code || !std::equal(std::begin(Saves), std::end(Saves), Code + Cursor) ||
      readLE<uint32_t>(Code + Cursor + 4) !=
          Image.Base + Image.DynInfo.SecurityCookieRVA ||
      Code[Cursor + 8] != 0x33 || Code[Cursor + 9] != 0xc4 ||
      Code[Cursor + 10] != 0x50 || Code[Cursor + 11] != 0x8d ||
      (Code[Cursor + 12] != 0x44 && Code[Cursor + 12] != 0x84) ||
      Code[Cursor + 13] != 0x24)
    return false;
  const va_t CookieOperand = Entry + Cursor + 4;
  int64_t NodeOffset = int8_t(Code[Cursor + 14]);
  if (Code[Cursor + 12] == 0x84) {
    Code = Image.readVA(Entry, Cursor + 18);
    if (!Code)
      return false;
    NodeOffset = readLE<int32_t>(Code + Cursor + 14);
    Cursor += 18;
  } else
    Cursor += 15;
  if (NodeOffset != Allocation + 16)
    return false;
  Code = Image.readVA(Entry, Cursor + 6);
  return Code && Code[Cursor] == 0x64 && Code[Cursor + 1] == 0xa3 &&
         readLE<uint32_t>(Code + Cursor + 2) == 0 &&
         readImmutablePE32CodeBytes(Image, Entry, Cursor + 6,
                                    {Entry + 6 + AlignSize, CookieOperand})
             .has_value();
}

} // namespace

std::optional<va_t> alignedCxxInstallEntry(const BinaryImage &Image,
                                           const InstallSite &Site) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Site.Identity.CxxFuncInfoVA ||
      Site.TryLevel != -1 || Image.Base > UINT32_MAX - 3 ||
      !Image.DynInfo.SecurityCookieRVA ||
      Image.DynInfo.SecurityCookieRVA > UINT32_MAX - 3 - Image.Base ||
      !Image.readVA(Image.Base + Image.DynInfo.SecurityCookieRVA, 4))
    return std::nullopt;
  for (unsigned AlignSize : {3u, 6u}) {
    const unsigned Prefix = 10 + AlignSize;
    if (Site.InstallVA >= Prefix &&
        matchesEntry(Image, Site, Site.InstallVA - Prefix, AlignSize))
      return Site.InstallVA - Prefix;
  }
  return std::nullopt;
}

} // namespace neverd::coff_loader::registration_detail
