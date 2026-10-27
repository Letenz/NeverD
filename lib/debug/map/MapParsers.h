//===- MapParsers.h - Linker map format parsers (internal) --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations for format-specific linker map parsers.
/// Used by LLDMapLoader to dispatch to the correct parser.
///
/// This header is an implementation detail of the debug map/ library and
/// should NOT be included by code outside lib/debug/map/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_MAP_MAPPARSERS_H
#define NEVERD_DEBUG_MAP_MAPPARSERS_H

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/MapFile.h"

#include "llvm/ADT/StringRef.h"

#include <map>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// Format detection helpers (shared by LLDMapLoader and per-format parsers)
//===----------------------------------------------------------------------===//

inline bool isELFMapHeader(llvm::StringRef Line) {
  return Line.trim().starts_with("VMA") && Line.contains("LMA");
}

inline bool isLLDMapHeader(llvm::StringRef Line) {
  llvm::StringRef Trimmed = Line.trim();
  return Trimmed.starts_with("Address") && Trimmed.contains("Align") &&
         Trimmed.contains("Out") && !Trimmed.contains("Publics");
}

inline bool isGNUldMapHeader(llvm::StringRef Line) {
  return Line.trim().starts_with("Linker script and memory map");
}

/// Whether \p Name is a name a disassembler made up from \p Addr
/// (MapAutomaticNames.def), which says no more than the address.
bool isAutomaticNameOf(llvm::StringRef Name, va_t Addr);

//===----------------------------------------------------------------------===//
// Per-format parsers
//===----------------------------------------------------------------------===//

// Each parser also lists the output sections the MAP places in \p Sections,
// when given one.

/// ELF-style map parser (ld.lld --Map=).
void parseELFMap(llvm::StringRef Content,
                 std::map<va_t, FunctionSym> &Functions,
                 std::vector<MapSection> *Sections = nullptr);

/// COFF /lldmap parser (lld-link /lldmap:).  Its sections are listed at
/// their image-relative addresses.
void parseCOFFLLDMap(llvm::StringRef Content,
                     std::map<va_t, FunctionSym> &Functions, uint64_t ImageBase,
                     std::vector<MapSection> *Sections = nullptr);

/// MachO-style map parser (ld64.lld -map).
void parseMachOMap(llvm::StringRef Content,
                   std::map<va_t, FunctionSym> &Functions,
                   std::vector<MapSection> *Sections = nullptr);

/// GNU ld map parser (ld --Map=).
void parseGNUldMap(llvm::StringRef Content,
                   std::map<va_t, FunctionSym> &Functions,
                   std::vector<MapSection> *Sections = nullptr);

} // namespace neverd

#endif // NEVERD_DEBUG_MAP_MAPPARSERS_H
