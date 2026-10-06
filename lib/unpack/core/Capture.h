//===- Capture.h - An observed image and how to rebuild it ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_CORE_CAPTURE_H
#define NEVERD_UNPACK_CORE_CAPTURE_H

#include "UnpackInternal.h"

#include <map>

namespace neverd::unpack {
/// One export identity a pointer-sized cell can name.
struct ExportBinding {
  std::string Module, Name;
  std::optional<uint16_t> Ordinal;
};

/// The image of a stopped process at the transfer where its entry was
/// established. Nothing in it is container specific.
struct Capture {
  /// The address the image was observed at.
  uint64_t Base;
  uint64_t EntryRVA;
  EntrySource Source;
  /// The image extent at the transfer and as the guest loader mapped it.
  std::vector<uint8_t> Memory, Baseline;
  /// Guest permissions of each page of the extent at the transfer; zero if
  /// the page was not mapped.
  std::vector<uint8_t> PageAccess;
  /// Entry addresses of every export the guest loader can bind.
  std::map<uint64_t, ExportBinding> Exports;
};

/// A record of the recovered program that replaces one the input's headers
/// name. A protector carries its own copy of metadata the system loader must
/// see before anything is unpacked; the program's record is found in the
/// observed memory. \p Kind is the container's own index for the record.
struct MetadataOverride {
  uint32_t Kind;
  uint64_t RVA, Size;
};

/// What a protector module adds to a rebuild beyond the observed memory.
struct RebuildPlan {
  std::vector<MetadataOverride> Metadata;
};

struct RebuiltImage {
  std::vector<uint8_t> File;
  std::vector<UnpackedSection> Sections;
  std::vector<UnpackedImport> Imports;
};
} // namespace neverd::unpack
#endif
