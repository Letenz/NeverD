//===- PEDriverRelocations.h - Retained loader fields ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve authenticated loader fields in diagnostic kernel snapshots.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_FORMAT_PE_PEDRIVERRELOCATIONS_H
#define NEVERD_UNPACK_FORMAT_PE_PEDRIVERRELOCATIONS_H

#include "PEImage.h"

#include <utility>

namespace neverd::unpack::pe {
struct DriverRelocationMetadata {
  llvm::object::data_directory Directory{};
  std::vector<uint32_t> Fields;
};
/// Append only source-bound, never-overwritten DIR64 fields that formatting
/// did not replace. This supplies no generated-pointer coverage or recovery
/// permission; the kernel runtime dependency must remain in force.
llvm::Expected<DriverRelocationMetadata> retainDriverRelocations(
    const Image &Input, const Capture &Observed, llvm::ArrayRef<uint8_t> Memory,
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> ReplacedRanges,
    uint64_t MetadataRVA, std::vector<uint8_t> &Metadata);
} // namespace neverd::unpack::pe
#endif
