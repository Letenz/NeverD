//===- DebugInfoDiscovery.cpp - Locate and load debug info ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Chooses which debug format applies to an image, finds the file that carries
/// it, and publishes the resulting names into the image's symbol table.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/DebugInfoDiscovery.h"

#include "neverd/Common.h"
#include "neverd/debug/DWARFLoader.h"
#include "neverd/debug/LLDMapLoader.h"
#include "neverd/debug/MSVCMapLoader.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/StringLiteralSymbols.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <span>
#include <system_error>

#define DEBUG_TYPE "neverd-debug-discovery"

namespace neverd {

namespace {

/// Record as pointers to data the relocated pointers that the image took for
/// pointers to code but whose targets \p Dbg places in data: at a data
/// symbol, the image's own among them (\p DataByAddr), or after one that no
/// function follows.
void recordDebugDataPointerTargets(BinaryImage &Img, const DebugContext &Dbg,
                                   const std::map<va_t, size_t> &DataByAddr) {
  recordDataPointerTargets(Img, [&](va_t Addr) {
    if (DataByAddr.count(Addr) != 0)
      return true;
    return (Dbg.resolveDataObject(Addr) || Dbg.precedingDataSymbol(Addr)) &&
           !Dbg.resolveFunction(Addr);
  });
}

/// Drop the functions the loader only guessed after padding where \p Dbg
/// names a data object, the image's own data symbols (\p DataByAddr) among
/// them: a string literal that an executable section keeps begins no
/// function, however its first bytes read as a prologue.
void dropDebugDataBoundaryGuesses(BinaryImage &Img, const DebugContext &Dbg,
                                  const std::map<va_t, size_t> &DataByAddr) {
  const auto IsGuess = [](const Symbol &Sym) {
    return Sym.IsFunc && Sym.IsBoundaryGuess && isSynthesizedFuncName(Sym.Name);
  };
  // What else says a function begins there, which the data does not refute.
  std::set<va_t> Confirmed;
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && !IsGuess(Sym))
      Confirmed.insert(Sym.Addr);
  for (const auto &[Start, End] : Img.KnownCodeRanges)
    if (!Img.ContinuationCodeStarts.count(Start))
      Confirmed.insert(Start);
  std::set<va_t> Refuted;
  for (const Symbol &Sym : Img.Symbols)
    if (IsGuess(Sym) && !Confirmed.count(Sym.Addr) &&
        (DataByAddr.count(Sym.Addr) != 0 ||
         (Dbg.resolveDataObject(Sym.Addr) && !Dbg.resolveFunction(Sym.Addr))))
      Refuted.insert(Sym.Addr);
  if (Refuted.empty())
    return;
  llvm::erase_if(Img.Symbols, [&](const Symbol &Sym) {
    return IsGuess(Sym) && Refuted.count(Sym.Addr) != 0;
  });
}

bool isReadableFile(const std::filesystem::path &P) {
  if (P.empty())
    return false;
  std::error_code EC;
  return std::filesystem::is_regular_file(P, EC);
}

void addCandidate(std::vector<std::filesystem::path> &Out,
                  std::filesystem::path Candidate) {
  if (Candidate.empty())
    return;
  if (std::find(Out.begin(), Out.end(), Candidate) == Out.end())
    Out.push_back(std::move(Candidate));
}

/// Companion files that hold the DWARF stripped out of an ELF image, in the
/// layouts objcopy --only-keep-debug and the distro debug packages produce.
std::vector<std::filesystem::path>
splitDwarfCandidates(const std::filesystem::path &BinaryPath) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();
  const auto Name = BinaryPath.filename();

  auto Companion = BinaryPath;
  Companion += ".debug";
  addCandidate(Out, std::move(Companion));
  Companion = BinaryPath;
  Companion.replace_extension(".debug");
  addCandidate(Out, std::move(Companion));
  addCandidate(Out, Dir / ".debug" / Name);
  return Out;
}

std::vector<std::filesystem::path>
mapCandidates(const std::filesystem::path &BinaryPath) {
  std::vector<std::filesystem::path> Out;
  const std::filesystem::path Dir = BinaryPath.parent_path();

  auto Companion = BinaryPath;
  Companion.replace_extension(".map");
  addCandidate(Out, std::move(Companion));
  Companion = BinaryPath;
  Companion += ".map";
  addCandidate(Out, std::move(Companion));
  return Out;
}

DebugInfoResult loadPDB(const std::filesystem::path &P, const BinaryImage &Img,
                        const LoadProgress &Progress,
                        bool AllowMissingIdentity = false) {
  DebugInfoResult R;
  auto CtxOr = AllowMissingIdentity
                   ? PDBDebugContext::loadUserSelected(P, Img, Progress)
                   : PDBDebugContext::load(P, Img, Progress);
  if (!CtxOr) {
    R.Error = llvm::toString(CtxOr.takeError());
    return R;
  }
  auto Ctx = std::move(*CtxOr);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::PDB;
    R.Path = P;
  } else {
    R.Error = "no function symbols in " + pathToUTF8(P);
  }
  return R;
}

DebugInfoResult loadDWARF(const std::filesystem::path &P, BinaryFormat Format,
                          DWARFLoadTrust Trust,
                          std::span<const uint8_t> ExpectedImageBytes = {}) {
  DebugInfoResult R;
  auto Ctx = DWARFDebugContext::load(P, Format, Trust, ExpectedImageBytes);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::DWARF;
    R.Path = P;
  }
  return R;
}

DebugInfoResult loadMap(const std::filesystem::path &P,
                        const BinaryImage &Img) {
  DebugInfoResult R;

  // A COFF /MAP states addresses as segment:offset, so it only resolves
  // against an image base, and link.exe appends line-number tables the other
  // dialects have no equivalent for.  MSVCMapDebugContext is the variant that
  // reads both.  The remaining four dialects carry whole addresses and go
  // through the auto-detecting loader, which also covers a PE linked with
  // lld's /lldmap.
  if (Img.Format == BinaryFormat::COFF) {
    auto MSVCCtx = MSVCMapDebugContext::load(P, Img.Base);
    if (MSVCCtx && MSVCCtx->hasInfo()) {
      R.Context = std::move(MSVCCtx);
      R.Kind = DebugInfoKind::Map;
      R.Path = P;
      return R;
    }
  }

  auto Ctx = LLDMapDebugContext::load(P, Img.isCOFF() ? Img.Base : 0);
  if (Ctx && Ctx->hasInfo()) {
    R.Context = std::move(Ctx);
    R.Kind = DebugInfoKind::Map;
    R.Path = P;
  }
  return R;
}

} // anonymous namespace

const char *debugInfoKindName(DebugInfoKind Kind) {
  switch (Kind) {
  case DebugInfoKind::None:
    return "none";
  case DebugInfoKind::DWARF:
    return "dwarf";
  case DebugInfoKind::PDB:
    return "pdb";
  case DebugInfoKind::Map:
    return "map";
  }
  return "none";
}

DebugInfoResult loadDebugInfo(const std::filesystem::path &BinaryPath,
                              const BinaryImage &Img,
                              const DebugInfoRequest &Req,
                              const LoadProgress &Progress) {
  DebugInfoResult Result;
  if (!Req.Enabled)
    return Result;

  if (!Req.PDBPath.empty()) {
    if (!isReadableFile(Req.PDBPath)) {
      Result.Error = "PDB file not found: " + pathToUTF8(Req.PDBPath);
      return Result;
    }
    Result = loadPDB(Req.PDBPath, Img, Progress, Req.AllowMissingPDBIdentity);
    if (!Result && Result.Error.empty())
      Result.Error = "no function symbols in " + pathToUTF8(Req.PDBPath);
    return Result;
  }

  if (!Req.MapPath.empty()) {
    if (!isReadableFile(Req.MapPath)) {
      Result.Error = "MAP file not found: " + pathToUTF8(Req.MapPath);
      return Result;
    }
    Result = loadMap(Req.MapPath, Img);
    if (!Result)
      Result.Error = "no function symbols in " + pathToUTF8(Req.MapPath);
    return Result;
  }

  // EVM and SBF describe themselves through their own metadata — an ABI JSON,
  // an Anchor IDL — and no producer emits DWARF, PDB, or a linker MAP for
  // either.  Probing for those files would only cost stat calls.
  if (Img.Arch == Arch::EVM || Img.Arch == Arch::SBF)
    return Result;

  switch (Img.Format) {
  case BinaryFormat::COFF:
    if (!Req.SearchPDB)
      break;
    for (const PDBCandidate &C :
         pdbCandidates(BinaryPath, Img, symbolPath(Req.SymbolPath))) {
      if (!isReadableFile(C.Path))
        continue;
      DebugInfoResult Attempt = loadPDB(C.Path, Img, Progress);
      if (Attempt)
        return Attempt;
      LLVM_DEBUG(llvm::dbgs()
                 << "debug-discovery: rejected PDB candidate "
                 << pathToUTF8(C.Path) << (Attempt.Error.empty() ? "" : ": ")
                 << Attempt.Error << "\n");
    }
    break;

  case BinaryFormat::ELF:
  case BinaryFormat::MachO:
    // Covers in-image .debug_info and, for Mach-O, an adjacent .dSYM bundle.
    Result =
        loadDWARF(BinaryPath, Img.Format, DWARFLoadTrust::InImage, Img.Raw);
    if (Result)
      return Result;
    for (const std::filesystem::path &C : splitDwarfCandidates(BinaryPath)) {
      if (!isReadableFile(C))
        continue;
      // Split ELF/Mach-O candidates remain useful for names and lines, but
      // cannot publish object extents until build-id/debuglink identity is
      // authenticated.  dSYM discovery from the primary Mach-O is handled by
      // DWARFDebugContext and separately verifies LC_UUID.
      Result =
          loadDWARF(C, Img.Format, DWARFLoadTrust::UnauthenticatedCompanion);
      if (Result)
        return Result;
    }
    break;

  default:
    break;
  }

  // A MAP carries names and addresses but no types, source lines, or build
  // identity, so it is the last resort: it exists for a stripped binary whose
  // build kept nothing else.
  for (const std::filesystem::path &C : mapCandidates(BinaryPath)) {
    if (!isReadableFile(C))
      continue;
    Result = loadMap(C, Img);
    if (Result)
      return Result;
  }

  return Result;
}

unsigned applyDebugSymbols(BinaryImage &Img, const DebugContext &Dbg) {
  const std::vector<FunctionSym> DbgFuncs = Dbg.allFunctions();
  if (Dbg.hasAuthenticatedObjectExtents()) {
    for (const DataObjectSym &Object : Dbg.allDataObjects()) {
      if (Object.Addr == InvalidVA || Object.Size == 0 ||
          Object.Size > InvalidVA - Object.Addr)
        continue;
      Img.ExactDataObjects.push_back(ExactDataObjectExtent{
          Object.Addr, Object.Size, ExactDataObjectEvidence::AuthenticatedDebug,
          Object.IsBuffer ? ExactDataObjectPrecision::TypedBuffer
                          : ExactDataObjectPrecision::TypedNonBuffer});
    }
  }

  // Names-only data publics (Phase A PDB / MAP) must still name image
  // objects.  Size 0 is intentional: extents stay unauthenticated.
  // Index once: a large MAP can carry tens of thousands of data publics.
  std::map<va_t, size_t> DataByAddr;
  for (size_t I = 0; I < Img.Symbols.size(); ++I) {
    if (!Img.Symbols[I].IsFunc)
      DataByAddr.try_emplace(Img.Symbols[I].Addr, I);
  }
  // The objects that may lie among code.  A literal, which its symbol names
  // by its text, reads as that text rather than as an object.
  std::vector<va_t> DataObjects;
  for (const DataObjectSym &Object : Dbg.allDataObjects()) {
    if (Object.Name.empty() || Object.Addr == InvalidVA)
      continue;
    if (!isStringLiteralName(Object.Name))
      DataObjects.push_back(Object.Addr);
    auto It = DataByAddr.find(Object.Addr);
    if (It != DataByAddr.end()) {
      Symbol &Existing = Img.Symbols[It->second];
      if (Existing.Name.empty() || Existing.Origin < NameOrigin::Stated) {
        Existing.Name = Object.Name;
        Existing.Origin = NameOrigin::Stated;
      }
      continue;
    }
    Symbol New;
    New.Name = Object.Name;
    New.Addr = Object.Addr;
    New.Size = Object.Size;
    New.IsFunc = false;
    DataByAddr.emplace(Object.Addr, Img.Symbols.size());
    Img.Symbols.push_back(std::move(New));
  }

  // A guessed function that a data object refutes claims no pointer to it
  // as one to code, so the guesses go first.
  if (DbgFuncs.empty()) {
    dropDebugDataBoundaryGuesses(Img, Dbg, DataByAddr);
    recordDebugDataPointerTargets(Img, Dbg, DataByAddr);
    recordDataInCode(Img, DataObjects);
    return 0;
  }

  // Index the function symbols once rather than scanning the table per debug
  // entry; both sides run to tens of thousands of entries on a real image.
  std::map<va_t, size_t> ByAddr;
  for (size_t I = 0; I < Img.Symbols.size(); ++I) {
    const Symbol &S = Img.Symbols[I];
    if (!S.IsFunc)
      continue;
    auto [It, Inserted] = ByAddr.try_emplace(S.Addr, I);
    // One address can carry both a stated name and a placeholder, when a
    // discovery pass minted `sub_` for code the symbol table also describes.
    // The stated one is what decides whether debug info may speak here.
    if (!Inserted && Img.Symbols[It->second].Origin < S.Origin)
      It->second = I;
  }

  unsigned Applied = 0;
  for (const FunctionSym &FS : DbgFuncs) {
    if (FS.Name.empty() || FS.Addr == 0)
      continue;

    auto It = ByAddr.find(FS.Addr);
    if (It == ByAddr.end()) {
      Symbol New;
      New.Name = FS.Name;
      New.Addr = FS.Addr;
      New.Size = FS.Size;
      New.IsFunc = true;
      Img.Symbols.push_back(std::move(New));
      ByAddr.emplace(FS.Addr, Img.Symbols.size() - 1);
      ++Applied;
      continue;
    }

    Symbol &Existing = Img.Symbols[It->second];
    if (Existing.Origin >= NameOrigin::Stated && !Existing.Name.empty())
      continue;
    Existing.Name = FS.Name;
    Existing.Origin = NameOrigin::Stated;
    if (Existing.Size == 0)
      Existing.Size = FS.Size;
    ++Applied;
  }

  dropDebugDataBoundaryGuesses(Img, Dbg, DataByAddr);
  recordDebugDataPointerTargets(Img, Dbg, DataByAddr);
  recordDataInCode(Img, DataObjects);
  LLVM_DEBUG(llvm::dbgs() << "debug-discovery: published " << Applied << " of "
                          << DbgFuncs.size() << " debug function symbols\n");
  return Applied;
}

} // namespace neverd
