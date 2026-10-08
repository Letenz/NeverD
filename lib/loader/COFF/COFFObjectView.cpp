//===- COFFObjectView.cpp - Linked PE reader projection ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFObjectView.h"

#include "neverd/object/PELayout.h"

#include "llvm/ADT/STLExtras.h"

#include <cstring>
#include <utility>
#include <vector>

namespace neverd::coff_loader {
namespace {
using Range = std::pair<uint64_t, uint64_t>;

bool disjoint(std::vector<Range> &Ranges) {
  llvm::sort(Ranges);
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first < Ranges[I - 1].second)
      return false;
  return true;
}
} // namespace

llvm::Expected<COFFObjectView>
COFFObjectView::create(llvm::MemoryBufferRef Input) {
  COFFObjectView View;
  const auto Bytes = llvm::arrayRefFromStringRef(Input.getBuffer());
  // Header inspection is read-only. Mutations below use owned storage only.
  const auto Headers =
      locatePEHeaders(const_cast<uint8_t *>(Bytes.data()), Bytes.size());
  bool NeedsProjection = false;
  forEachPESection(Headers, [&](const PESectionFields &S, uint16_t) {
    NeedsProjection |= !S.VirtualSize && S.SizeOfRawData;
  });
  if (NeedsProjection) {
    const uint64_t HeaderSize = getPESizeOfHeaders(Headers);
    const uint64_t ImageSize = getPESizeOfImage(Headers);
    const uint64_t TableEnd =
        Headers.SectionTable - Bytes.data() +
        uint64_t(Headers.NumSections) * sizeof(llvm::object::coff_section);
    bool Valid = HeaderSize >= TableEnd && HeaderSize <= Bytes.size() &&
                 HeaderSize <= ImageSize;
    std::vector<Range> Virtual{{0, HeaderSize}}, Raw{{0, HeaderSize}};
    forEachPESection(Headers, [&](const PESectionFields &S, uint16_t) {
      const uint64_t Size =
          getPESectionContentSize(S.VirtualSize, S.SizeOfRawData);
      const uint64_t RVA = S.VirtualAddress, Offset = S.PointerToRawData;
      Valid &= RVA <= ImageSize && Size <= ImageSize - RVA &&
               Offset <= Bytes.size() &&
               S.SizeOfRawData <= Bytes.size() - Offset;
      if (Size)
        Virtual.emplace_back(RVA, RVA + Size);
      if (S.SizeOfRawData)
        Raw.emplace_back(Offset, Offset + S.SizeOfRawData);
    });
    // A reader projection is not authority to resolve aliased metadata or
    // to expose projected header bytes as section contents.
    if (!Valid || !disjoint(Virtual) || !disjoint(Raw))
      return llvm::createStringError(
          "coff: zero-VirtualSize section has invalid or overlapping extents");
    View.Storage = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(
        Bytes.size(), Input.getBufferIdentifier());
    if (!View.Storage)
      return llvm::createStringError("coff: could not allocate reader view");
    std::memcpy(View.Storage->getBufferStart(), Bytes.data(), Bytes.size());
    auto Projected = locatePEHeaders(
        reinterpret_cast<uint8_t *>(View.Storage->getBufferStart()),
        Bytes.size());
    auto *Sections =
        reinterpret_cast<llvm::object::coff_section *>(Projected.SectionTable);
    for (unsigned I = 0; I < Projected.NumSections; ++I)
      Sections[I].VirtualSize = getPESectionContentSize(
          Sections[I].VirtualSize, Sections[I].SizeOfRawData);
    Input = View.Storage->getMemBufferRef();
  }
  auto Object = llvm::object::COFFObjectFile::create(Input);
  if (!Object)
    return Object.takeError();
  View.Object = std::move(*Object);
  return View;
}

} // namespace neverd::coff_loader
