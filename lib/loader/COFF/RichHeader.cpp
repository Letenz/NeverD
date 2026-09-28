//===- RichHeader.cpp - PE Rich header @comp.id records --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the Rich header the way richprint does
/// (https://github.com/dishather/richprint/blob/49f2dc93504db4a669c968fa80eb9b34591cb557/richprint.cpp,
/// Copyright (c) 2015-2024 dishather, BSD-2-Clause, see
/// LICENSES/richprint.txt): find "Rich" before the PE header, take the key
/// after it, find the "DanS" marker the key encodes, and read the records
/// between them.  NeverD adds the checksum the key is (Daniel Pistelli,
/// "Microsoft's Rich Signature (undocumented)"), so that a block edited after
/// linking is not believed.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/RichHeader.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

using namespace neverd;

namespace {

struct RichProductRecord {
  uint16_t ProdId;
  RichTool Tool;
  unsigned Year;
  const char *Description;
};

struct RichBuildRecord {
  uint16_t ProdId;
  uint16_t Build;
  RichTool Tool;
  unsigned Year;
  bool Interpolated;
  const char *Description;
};

#include "RichCompIds.inc"

constexpr uint32_t RichMarker = 0x68636952; // "Rich"
constexpr uint32_t DanSMarker = 0x536E6144; // "DanS"
constexpr uint32_t DosHeaderSize = 0x40;
constexpr uint32_t PEOffsetField = 0x3C;

uint32_t readLE32(llvm::ArrayRef<uint8_t> File, size_t Off) {
  return llvm::support::endian::read32le(File.data() + Off);
}

uint32_t rotateLeft(uint32_t Value, uint32_t Count) {
  Count &= 31;
  return Count == 0 ? Value : (Value << Count) | (Value >> (32 - Count));
}

/// The linker's checksum: the block's offset, every byte before the block
/// except e_lfanew, each rotated by its offset, and every record rotated by
/// its count.
uint32_t richChecksum(llvm::ArrayRef<uint8_t> File, uint32_t Offset,
                      const std::vector<RichEntry> &Entries) {
  uint32_t Sum = Offset;
  for (uint32_t I = 0; I < Offset; ++I) {
    if (I >= PEOffsetField && I < PEOffsetField + 4)
      continue;
    Sum += rotateLeft(File[I], I);
  }
  for (const RichEntry &E : Entries) {
    const uint32_t CompId = (uint32_t(E.ProdId) << 16) | E.Build;
    Sum += rotateLeft(CompId, E.Count);
  }
  return Sum;
}

bool isCodeTool(RichTool Tool) {
  switch (Tool) {
  case RichTool::C:
  case RichTool::Cpp:
  case RichTool::Asm:
  case RichTool::LtcgC:
  case RichTool::LtcgCpp:
  case RichTool::PogoInstrumentC:
  case RichTool::PogoInstrumentCpp:
  case RichTool::PogoOptimizeC:
  case RichTool::PogoOptimizeCpp:
    return true;
  default:
    return false;
  }
}

} // namespace

std::optional<RichHeader>
neverd::decodeRichHeader(llvm::ArrayRef<uint8_t> File) {
  if (File.size() < DosHeaderSize || File[0] != 'M' || File[1] != 'Z')
    return std::nullopt;
  const uint32_t PEOffset = readLE32(File, PEOffsetField);
  if (PEOffset <= DosHeaderSize || PEOffset > File.size())
    return std::nullopt;

  // The block sits in the DOS stub area, aligned to four bytes.
  std::optional<uint32_t> RichAt;
  for (uint32_t Off = DosHeaderSize; Off + 8 <= PEOffset; Off += 4)
    if (readLE32(File, Off) == RichMarker) {
      RichAt = Off;
      break;
    }
  if (!RichAt)
    return std::nullopt;
  const uint32_t Key = readLE32(File, *RichAt + 4);

  std::optional<uint32_t> DanSAt;
  for (uint32_t Off = DosHeaderSize; Off + 4 <= *RichAt; Off += 4)
    if ((readLE32(File, Off) ^ Key) == DanSMarker) {
      DanSAt = Off;
      break;
    }
  // The marker is followed by three zero words, then by whole records.
  if (!DanSAt || *DanSAt + 16 > *RichAt || (*RichAt - *DanSAt - 16) % 8 != 0)
    return std::nullopt;
  for (uint32_t Pad = 1; Pad <= 3; ++Pad)
    if ((readLE32(File, *DanSAt + 4 * Pad) ^ Key) != 0)
      return std::nullopt;

  RichHeader Header;
  Header.Offset = *DanSAt;
  Header.Key = Key;
  for (uint32_t Off = *DanSAt + 16; Off < *RichAt; Off += 8) {
    const uint32_t CompId = readLE32(File, Off) ^ Key;
    RichEntry E;
    E.ProdId = static_cast<uint16_t>(CompId >> 16);
    E.Build = static_cast<uint16_t>(CompId & 0xFFFF);
    E.Count = readLE32(File, Off + 4) ^ Key;
    Header.Entries.push_back(E);
  }
  Header.ChecksumMatches =
      richChecksum(File, Header.Offset, Header.Entries) == Key;
  return Header;
}

RichToolInfo neverd::describeRichEntry(uint16_t ProdId, uint16_t Build) {
  RichToolInfo Info;
  const auto *Product = std::lower_bound(
      std::begin(RichProducts), std::end(RichProducts), ProdId,
      [](const RichProductRecord &R, uint16_t Id) { return R.ProdId < Id; });
  const bool HasProduct =
      Product != std::end(RichProducts) && Product->ProdId == ProdId;
  if (HasProduct) {
    Info.Tool = Product->Tool;
    Info.Description = Product->Description;
  }

  const auto *First = std::lower_bound(
      std::begin(RichBuilds), std::end(RichBuilds), ProdId,
      [](const RichBuildRecord &R, uint16_t Id) { return R.ProdId < Id; });
  const auto *Last = std::upper_bound(
      First, std::end(RichBuilds), ProdId,
      [](uint16_t Id, const RichBuildRecord &R) { return Id < R.ProdId; });
  if (First != Last) {
    // The last listed build at or before this one; failing that, the first.
    const auto *At = std::upper_bound(
        First, Last, Build,
        [](uint16_t B, const RichBuildRecord &R) { return B < R.Build; });
    const RichBuildRecord &Near = At == First ? *First : *(At - 1);
    Info.VisualStudioYear = Near.Year;
    Info.ExactBuild = Near.Build == Build;
    if (Info.ExactBuild)
      Info.Description = Near.Description;
    if (!HasProduct)
      Info.Tool = Near.Tool;
    return Info;
  }
  // Without build records, a product id names its release only when it
  // belongs to one release; "VS2015+" spans every release since.
  if (HasProduct && !llvm::StringRef(Product->Description).contains("+ ("))
    Info.VisualStudioYear = Product->Year;
  return Info;
}

std::vector<unsigned> neverd::richToolsetYears(const RichHeader &Header) {
  if (!Header.ChecksumMatches)
    return {};
  std::set<unsigned> Linkers, Code;
  for (const RichEntry &E : Header.Entries) {
    const RichToolInfo Info = describeRichEntry(E.ProdId, E.Build);
    if (Info.VisualStudioYear == 0)
      continue;
    if (Info.Tool == RichTool::Linker)
      Linkers.insert(Info.VisualStudioYear);
    else if (isCodeTool(Info.Tool))
      Code.insert(Info.VisualStudioYear);
  }
  const std::set<unsigned> &Years = Linkers.empty() ? Code : Linkers;
  return std::vector<unsigned>(Years.begin(), Years.end());
}
