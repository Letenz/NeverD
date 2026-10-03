//===- MachOExecutionImage.h - Original Mach-O execution facts --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_MACHO_MACHOEXECUTIONIMAGE_H
#define NEVERD_LOADER_MACHO_MACHOEXECUTIONIMAGE_H

#include "neverd/loader/BinaryImageModel.h"

#include <filesystem>

namespace neverd {
struct MachOExecutionSegment {
  uint32_t InitialProtection, MaximumProtection, Flags;
};
/// Format facts only. Guest platform, startup and service admission belong to
/// the OS model. Segment bytes are always original, never analysis fixups.
struct MachOExecutionImage {
  BinaryImage Image;
  uint32_t CPUType = 0, CPUSubtype = 0, FileType = 0, Flags = 0;
  std::vector<uint32_t> Platforms;
  std::vector<MachOExecutionSegment> Segments;
  std::vector<std::string> Interpreters, Dependencies;
  std::vector<uint32_t> OtherCommands;
  uint32_t MainEntries = 0, ThreadEntries = 0;
  uint64_t RequestedStackSize = 0;
  bool HasNonEntryThreadState = false, HasFixups = false;
  bool HasInitializers = false, HasTLS = false, Encrypted = false;
};

/// Bounded thin 64-bit parsing, without selecting a universal slice from the
/// host ISA or resolving imports through host libraries. The complete regular
/// file, including unmapped metadata and trailing bytes, must fit
/// FileByteLimit. Reads are bounded before parsing and do not retain a live
/// file mapping.
llvm::Expected<MachOExecutionImage>
loadMachOExecutionImage(const std::filesystem::path &Path,
                        uint64_t FileByteLimit);
/// Compatibility entry point with a 64 MiB input-file limit.
llvm::Expected<MachOExecutionImage>
loadMachOExecutionImage(const std::filesystem::path &Path);
} // namespace neverd
#endif
