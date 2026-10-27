//===- MapDebugContextBase.h - Shared base for MAP loaders -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common base class for MAP-file-based debug contexts (MSVC, LLD).
/// Provides shared resolveFunction / allFunctions / hasInfo
/// implementations backed by a std::map<va_t, FunctionSym>.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_MAPDEBUGCONTEXTBASE_H
#define NEVERD_DEBUG_MAPDEBUGCONTEXTBASE_H

#include "neverd/debug/DebugContext.h"

#include "llvm/ADT/StringRef.h"

#include <map>

namespace neverd {

struct MapImageLayout;

class MapDebugContextBase : public DebugContext {
public:
  ~MapDebugContextBase() override = default;

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override;
  std::optional<TypeSym> resolveType(uint64_t) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  std::vector<DataObjectSym> allDataObjects() const override;
  bool hasInfo() const override;

  static bool isCOFFMapHeader(llvm::StringRef Line);

  static bool parseSegOffset(llvm::StringRef Token, uint16_t &Seg,
                             uint64_t &Offset);

  /// Parse COFF /MAP content into \p Functions.  Shared between
  /// MSVCMapLoader and LLDMapLoader (which auto-detects this format).
  ///
  /// With \p Layout, the image the MAP is read against places the names: a
  /// public without link.exe's Rva+Base column (IDA's and Borland's MAPs)
  /// takes the reading of its segment:offset that puts the most names in the
  /// image, the image's sections say which names are code, an address of an
  /// image rebased since the link moves with it, and a name made up from its
  /// own address (MapAutomaticNames.def) is left out.
  static void
  parseCOFFMapContent(llvm::StringRef Content,
                      std::map<va_t, FunctionSym> &Functions,
                      uint64_t ImageBase = 0,
                      std::map<va_t, DataObjectSym> *DataObjects = nullptr,
                      const MapImageLayout *Layout = nullptr);

  /// With \p Layout, segment N is the image's Nth section, as link.exe
  /// numbers them.
  static void parseCOFFMapLineNumbers(llvm::StringRef Content,
                                      std::map<va_t, SourceLoc> &Locations,
                                      uint64_t ImageBase = 0,
                                      const MapImageLayout *Layout = nullptr);

protected:
  std::map<va_t, FunctionSym> Functions;
  std::map<va_t, DataObjectSym> DataObjects;
  std::map<va_t, SourceLoc> SourceLocations;
  bool Loaded = false;

  /// Infer function sizes from gaps between adjacent symbol addresses.
  /// Call after populating Functions.
  void inferFunctionSizes();
};

} // namespace neverd

#endif // NEVERD_DEBUG_MAPDEBUGCONTEXTBASE_H
