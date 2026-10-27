//===- LLDMapLoader.cpp - LLD linker MAP file loader ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Format detection and public API for linker MAP file loading.
/// Supports five formats:
///
/// 1. ELF-style  (ld.lld --Map=)     — ELFMapParser.cpp
/// 2. COFF /MAP  (lld-link /MAP:)    — MapDebugContextBase::parseCOFFMapContent
/// 3. COFF /lldmap (lld-link /lldmap:) — COFFLLDMapParser.cpp
/// 4. MachO-style (ld64.lld -map)    — MachOMapParser.cpp
/// 5. GNU ld     (ld --Map=)         — GNUldMapParser.cpp
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/LLDMapLoader.h"

#include "MapParsers.h"

#include "neverd/debug/MapDebugContextBase.h"
#include "neverd/support/FilePath.h"

#define DEBUG_TYPE "neverd-lld-map-loader"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

std::unique_ptr<LLDMapDebugContext>
LLDMapDebugContext::load(const std::filesystem::path &MapPath,
                         uint64_t ImageBase) {
  auto Ctx = std::unique_ptr<LLDMapDebugContext>(new LLDMapDebugContext());

  auto BufOr = llvm::MemoryBuffer::getFile(pathToUTF8(MapPath));
  if (!BufOr) {
    llvm::WithColor::warning()
        << "lld-map: cannot open " << pathToUTF8(MapPath) << "\n";
    return Ctx;
  }

  llvm::StringRef Content = (*BufOr)->getBuffer();
  const MapDialect Dialect = detectMapDialect(Content);
  switch (Dialect) {
  case MapDialect::LLD:
    parseELFMap(Content, Ctx->Functions);
    break;
  case MapDialect::MSVC:
    MapDebugContextBase::parseCOFFMapContent(Content, Ctx->Functions, ImageBase,
                                             &Ctx->DataObjects);
    break;
  case MapDialect::LLDCOFF:
    parseCOFFLLDMap(Content, Ctx->Functions, ImageBase);
    break;
  case MapDialect::LD64:
    parseMachOMap(Content, Ctx->Functions);
    break;
  case MapDialect::GNU:
    parseGNUldMap(Content, Ctx->Functions);
    break;
  }

  Ctx->inferFunctionSizes();
  Ctx->Loaded = !Ctx->Functions.empty();
  LLVM_DEBUG(llvm::dbgs() << "lld-map: loaded " << Ctx->Functions.size()
                          << " function symbols from "
                          << pathToUTF8(MapPath.filename()) << " ("
                          << mapDialectName(Dialect) << " style)\n");
  return Ctx;
}

} // namespace neverd
