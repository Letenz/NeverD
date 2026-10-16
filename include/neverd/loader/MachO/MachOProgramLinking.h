//===- MachOProgramLinking.h - Original execution fixups --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Bounded, complete linking facts for thin Mach-O execution images.
#ifndef NEVERD_LOADER_MACHO_MACHOPROGRAMLINKING_H
#define NEVERD_LOADER_MACHO_MACHOPROGRAMLINKING_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {
struct MachOProgramDependency {
  uint32_t Command;
  std::string Name;
  uint32_t CurrentVersion = 0, CompatibilityVersion = 0;
};
enum class MachOProgramFixupOrigin {
  ClassicRebase,
  ClassicBind,
  ClassicLazyBind,
  Chained
};
struct MachOProgramFixup {
  uint64_t Address = 0, Target = 0;
  std::string Symbol;
  int64_t Addend = 0;
  int32_t LibraryOrdinal = 0;
  bool Bind = false, WeakImport = false;
  MachOProgramFixupOrigin Origin = MachOProgramFixupOrigin::ClassicRebase;
};
struct MachOProgramExport {
  std::string Name;
  uint64_t Address = 0;
  bool ThreadLocal = false;
};
struct MachOProgramLinking {
  std::optional<MachOProgramDependency> Identity;
  std::vector<MachOProgramDependency> Dependencies;
  std::vector<MachOProgramFixup> Fixups;
  std::vector<MachOProgramExport> Exports;
  /// Conservative charge for decoded records, strings and reader indexes.
  uint64_t MetadataBytes = 0;
  uint64_t Records = 0;
};
/// Read original bytes, never analysis-patched pointers or host libraries.
/// Ordinary pointer streams, independently complete lazy pointer programs and
/// non-authenticated generic 64-bit chains are supported. One classic rebase
/// to local executable bytes may precede a lazy bind at the exact same slot;
/// both raw records are charged and one final binding is published.
/// Malformed, ambiguous, excessive and unsupported
/// inputs return an error, never a partial result. CheckPreparation is called
/// before parsing and during expansion; its caller-owned errors propagate
/// unchanged, without coupling the format reader to an execution runtime.
llvm::Expected<MachOProgramLinking>
readMachOProgramLinking(llvm::ArrayRef<uint8_t> Bytes,
                        uint64_t MetadataByteLimit,
                        llvm::function_ref<llvm::Error()> CheckPreparation);
} // namespace neverd
#endif
