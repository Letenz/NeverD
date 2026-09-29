//===- SignatureDB.cpp - Signature database manager -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureDB.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/InstructionFields.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace neverd;
using namespace neverd::sigs;

namespace {

#define NEVERD_SIGS_FILE_STRING(Name, Value)                                   \
  constexpr llvm::StringLiteral Name(Value);
#define NEVERD_SIGS_FILE_VALUE(Name, Value) constexpr unsigned Name = Value;
#include "SignatureFiles.def"

/// One source's pattern lines, parsed, and the bytes their modules keep.
struct ParsedSource {
  std::vector<PatternChunk> Chunks;
  std::unique_ptr<uint8_t[]> Bytes;
};

llvm::Expected<ParsedSource> parseSource(llvm::StringRef Text) {
  ParsedSource Source;
  Source.Chunks = PatternParser::splitChunks(Text);
  Source.Bytes = PatternParser::reserveBytes(Source.Chunks);
  PatternParser::parseChunks(Source.Chunks);
  if (llvm::Error Error = PatternParser::firstError(Source.Chunks))
    return std::move(Error);
  return std::move(Source);
}

/// The modules \p Chunks hold, in order; the names they point into move to
/// \p Names.
std::vector<StoredModule> takeModules(std::vector<PatternChunk> &Chunks,
                                      std::vector<PatternNames> &Names) {
  std::vector<StoredModule> Mods;
  for (PatternChunk &Chunk : Chunks) {
    Mods.insert(Mods.end(), Chunk.Modules.begin(), Chunk.Modules.end());
    Names.push_back(std::move(Chunk.Names));
  }
  return Mods;
}

} // namespace

void SignatureDB::commitSource(SigSource Src, std::vector<StoredModule> Mods) {
  Src.ModuleCount = Mods.size();
  auto Existing = std::find_if(
      LoadedFiles.begin(), LoadedFiles.end(),
      [&](const SigSource &Source) { return Source.Path == Src.Path; });
  if (Existing != LoadedFiles.end()) {
    const size_t Start = Existing->ModuleStart;
    Modules.erase(Modules.begin() + Start,
                  Modules.begin() + Start + Existing->ModuleCount);
    Modules.insert(Modules.begin() + Start, Mods.begin(), Mods.end());
    *Existing = std::move(Src);

    size_t ModuleStart = 0;
    for (SigSource &Source : LoadedFiles) {
      Source.ModuleStart = ModuleStart;
      ModuleStart += Source.ModuleCount;
    }
    Index.reset();
    clearMatches();
    return;
  }

  Src.ModuleStart = Modules.size();
  LoadedFiles.push_back(std::move(Src));
  Modules.insert(Modules.end(), Mods.begin(), Mods.end());
  Index.reset();
  clearMatches();
}

const SignatureMatcher::HashIndex &SignatureDB::index() {
  if (!Index) {
    Index = std::make_unique<SignatureMatcher::HashIndex>();
    Index->build(Modules);
  }
  return *Index;
}

const std::string &SignatureDB::libraryNameOf(size_t ModuleIndex) const {
  for (const SigSource &Src : LoadedFiles)
    if (ModuleIndex >= Src.ModuleStart &&
        ModuleIndex < Src.ModuleStart + Src.ModuleCount)
      return Src.LibraryName;
  static const std::string Empty;
  return Empty;
}

llvm::Error SignatureDB::loadFile(const std::filesystem::path &Path) {
  auto Ext = Path.extension().string();

  if (Ext == PatternExtension) {
    SigSource Src;
    Src.Path = Path.string();
    Src.LibraryName = libraryName(Path);
    const std::optional<SignatureCache::SourceIdentity> Identity =
        Cache ? SignatureCache::identify(Path) : std::nullopt;
    if (Identity) {
      if (std::optional<SignatureCache::Entry> Hit =
              Cache->load(Path, *Identity)) {
        Src.Mapping = std::move(Hit->Mapping);
        Src.Names.push_back(std::move(Hit->Names));
        commitSource(std::move(Src), std::move(Hit->Modules));
        return llvm::Error::success();
      }
    }
    auto BufferOrErr = llvm::MemoryBuffer::getFile(
        Path.string(), /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!BufferOrErr)
      return llvm::make_error<llvm::StringError>(
          "cannot open pattern file: " + Path.string(),
          llvm::inconvertibleErrorCode());
    auto SourceOrErr = parseSource((*BufferOrErr)->getBuffer());
    if (!SourceOrErr)
      return SourceOrErr.takeError();
    std::vector<StoredModule> Mods =
        takeModules(SourceOrErr->Chunks, Src.Names);
    Src.Bytes = std::move(SourceOrErr->Bytes);
    if (Identity)
      Cache->store(Path, *Identity, Mods);
    commitSource(std::move(Src), std::move(Mods));
    return llvm::Error::success();
  }

  return llvm::make_error<llvm::StringError>(
      "unsupported signature file format: " + Path.string(),
      llvm::inconvertibleErrorCode());
}

llvm::Error SignatureDB::loadPatternText(llvm::StringRef Text,
                                         llvm::StringRef LibraryName) {
  auto SourceOrErr = parseSource(Text);
  if (!SourceOrErr)
    return SourceOrErr.takeError();
  SigSource Src;
  Src.Path = LibraryName.str();
  Src.LibraryName = LibraryName.str();
  std::vector<StoredModule> Mods = takeModules(SourceOrErr->Chunks, Src.Names);
  Src.Bytes = std::move(SourceOrErr->Bytes);
  commitSource(std::move(Src), std::move(Mods));
  return llvm::Error::success();
}

llvm::Expected<std::vector<std::filesystem::path>>
SignatureDB::listDirectory(const std::filesystem::path &Dir) {
  std::error_code EC;
  if (!std::filesystem::exists(Dir, EC)) {
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature directory: " + Dir.string() + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
    return llvm::make_error<llvm::StringError>(
        "signature directory does not exist: " + Dir.string(),
        llvm::inconvertibleErrorCode());
  }
  if (!std::filesystem::is_directory(Dir, EC)) {
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature directory: " + Dir.string() + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
    return llvm::make_error<llvm::StringError>(
        "signature path is not a directory: " + Dir.string(),
        llvm::inconvertibleErrorCode());
  }

  // Collect all .pat files first, then parse in parallel.
  std::vector<std::filesystem::path> PatFiles;
  std::filesystem::directory_iterator It(Dir, EC);
  const std::filesystem::directory_iterator End;
  if (EC)
    return llvm::make_error<llvm::StringError>(
        "cannot enumerate signature directory: " + Dir.string() + ": " +
            EC.message(),
        llvm::inconvertibleErrorCode());
  while (It != End) {
    std::error_code TypeError;
    const bool IsRegular = It->is_regular_file(TypeError);
    if (TypeError)
      return llvm::make_error<llvm::StringError>(
          "cannot inspect signature entry: " + It->path().string() + ": " +
              TypeError.message(),
          llvm::inconvertibleErrorCode());
    if (IsRegular && It->path().extension() == PatternExtension.data())
      PatFiles.push_back(It->path());
    It.increment(EC);
    if (EC)
      return llvm::make_error<llvm::StringError>(
          "cannot enumerate signature directory: " + Dir.string() + ": " +
              EC.message(),
          llvm::inconvertibleErrorCode());
  }
  std::sort(PatFiles.begin(), PatFiles.end());
  return PatFiles;
}

std::string SignatureDB::libraryName(const std::filesystem::path &File) {
  const std::string Stem = File.stem().string();
  const size_t Dot = llvm::StringRef(Stem).rfind(PartMarker);
  if (Dot == std::string::npos || Dot == 0)
    return Stem;
  const llvm::StringRef Number =
      llvm::StringRef(Stem).drop_front(Dot + PartMarker.size());
  unsigned Part = 0, FirstDigit = 0;
  // Part 1 is the file under the library's own name; a part number is
  // written without leading zeros.
  if (Number.getAsInteger(NameNumberRadix, Part) || Part < FirstNumberedPart ||
      Number.take_front().getAsInteger(NameNumberRadix, FirstDigit) ||
      FirstDigit == 0)
    return Stem;
  return Stem.substr(0, Dot);
}

std::vector<std::filesystem::path>
SignatureDB::selectForImage(const BinaryImage &Img,
                            std::vector<std::filesystem::path> Files) {
  if (!Img.COFFRichHeader)
    return Files;
  const std::vector<unsigned> Years = richToolsetYears(*Img.COFFRichHeader);
  if (Years.empty())
    return Files;

  // "vs2026.pat" belongs to Visual Studio 2026; other names to no release.
  auto ReleaseOf = [](const std::filesystem::path &Path) {
    const std::string Stem = libraryName(Path);
    llvm::StringRef Digits = Stem;
    unsigned Year = 0;
    if (!Digits.consume_front(ReleasePrefix) ||
        Digits.size() != ReleaseYearDigits ||
        Digits.getAsInteger(NameNumberRadix, Year))
      return 0u;
    return Year;
  };
  std::vector<std::filesystem::path> Selected;
  bool KeptRelease = false;
  for (const std::filesystem::path &File : Files) {
    const unsigned Release = ReleaseOf(File);
    if (Release == 0) {
      Selected.push_back(File);
      continue;
    }
    if (std::find(Years.begin(), Years.end(), Release) != Years.end()) {
      Selected.push_back(File);
      KeptRelease = true;
    }
  }
  return KeptRelease ? Selected : Files;
}

std::optional<std::filesystem::path>
SignatureDB::treeDirectory(const BinaryImage &Img) {
  std::optional<llvm::StringRef> Format, Family;
  switch (Img.Format) {
#define NEVERD_SIGS_TREE_FORMAT(Name, Directory)                               \
  case BinaryFormat::Name:                                                     \
    Format = Directory;                                                        \
    break;
#include "SignatureFiles.def"
  default:
    break;
  }
  switch (Img.Arch) {
#define NEVERD_SIGS_TREE_ARCH(Name, Directory)                                 \
  case Arch::Name:                                                             \
    Family = Directory;                                                        \
    break;
#include "SignatureFiles.def"
  default:
    break;
  }
  if (!Format || !Family)
    return std::nullopt;
  return std::filesystem::path(Format->str()) / Family->str() /
         (Img.is64Bit() ? TreeDirectory64Bit : TreeDirectory32Bit).str();
}

llvm::Error SignatureDB::loadDirectory(const std::filesystem::path &Dir) {
  auto PatFiles = listDirectory(Dir);
  if (!PatFiles)
    return PatFiles.takeError();
  return loadFiles(*PatFiles);
}

llvm::Error
SignatureDB::loadFiles(const std::vector<std::filesystem::path> &PatFiles) {
  // A file the cache holds as it is now is mapped rather than parsed.  Every
  // entry is opened, and the pieces of all of them -- checking their blocks,
  // making their modules -- are shared out among the workers, as the chunks
  // of the files that are parsed are below.
  std::vector<std::optional<SignatureCache::SourceIdentity>> Identities(
      PatFiles.size());
  std::vector<std::optional<SignatureCache::PendingEntry>> Pending(
      PatFiles.size());
  if (Cache)
    neverd::parallelForEach(PatFiles.size(), [&](auto Claim, size_t Total) {
      for (size_t I = Claim(); I < Total; I = Claim()) {
        Identities[I] = SignatureCache::identify(PatFiles[I]);
        if (Identities[I])
          Pending[I] = Cache->open(PatFiles[I], *Identities[I]);
      }
    });
  std::vector<std::pair<size_t, size_t>> Pieces;
  for (size_t I = 0; I < PatFiles.size(); ++I)
    if (Pending[I])
      for (size_t Piece = 0; Piece < Pending[I]->pieces(); ++Piece)
        Pieces.emplace_back(I, Piece);
  std::vector<std::atomic<bool>> Damaged(PatFiles.size());
  neverd::parallelForEach(Pieces.size(), [&](auto Claim, size_t Total) {
    for (size_t W = Claim(); W < Total; W = Claim())
      if (!Pending[Pieces[W].first]->run(Pieces[W].second))
        Damaged[Pieces[W].first] = true;
  });
  std::vector<std::optional<SignatureCache::Entry>> Cached(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I)
    if (Pending[I] && !Damaged[I])
      Cached[I] = std::move(*Pending[I]).take();
  Pending.clear();

  // Every other file is cut into chunks of whole lines, and one pool of
  // workers parses the chunks of all of them: one large file keeps every
  // worker as busy as many small ones do.  A parsed chunk no longer refers to
  // its text, so a file is let go as soon as its last chunk is done.
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Buffers(PatFiles.size());
  std::vector<std::unique_ptr<uint8_t[]>> Bytes(PatFiles.size());
  std::vector<PatternChunk> Chunks;
  std::vector<size_t> FirstChunk(PatFiles.size() + 1), FileOf;
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    FirstChunk[I] = Chunks.size();
    if (Cached[I])
      continue;
    auto BufferOrErr =
        llvm::MemoryBuffer::getFile(PatFiles[I].string(), /*IsText=*/false,
                                    /*RequiresNullTerminator=*/false);
    if (!BufferOrErr)
      continue;
    Buffers[I] = std::move(*BufferOrErr);
    std::vector<PatternChunk> FileChunks =
        PatternParser::splitChunks(Buffers[I]->getBuffer());
    Bytes[I] = PatternParser::reserveBytes(FileChunks);
    Chunks.insert(Chunks.end(), std::make_move_iterator(FileChunks.begin()),
                  std::make_move_iterator(FileChunks.end()));
    FileOf.resize(Chunks.size(), I);
  }
  FirstChunk[PatFiles.size()] = Chunks.size();
  std::vector<std::atomic<size_t>> Unparsed(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I)
    Unparsed[I] = FirstChunk[I + 1] - FirstChunk[I];
  PatternParser::parseChunks(Chunks, [&](size_t Chunk) {
    if (--Unparsed[FileOf[Chunk]] == 0)
      Buffers[FileOf[Chunk]].reset();
  });

  // The first file that cannot be read or parsed, in order, fails the batch
  // and leaves the database as it was.
  size_t Count = 0;
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    if (Cached[I]) {
      Count += Cached[I]->Modules.size();
      continue;
    }
    const llvm::ArrayRef<PatternChunk> FileChunks(
        Chunks.data() + FirstChunk[I], Chunks.data() + FirstChunk[I + 1]);
    const bool Opened = FirstChunk[I] != FirstChunk[I + 1] || Buffers[I];
    llvm::Error Error =
        Opened ? PatternParser::firstError(FileChunks)
               : llvm::make_error<llvm::StringError>(
                     "cannot open pattern file: " + PatFiles[I].string(),
                     llvm::inconvertibleErrorCode());
    if (Error)
      return llvm::make_error<llvm::StringError>(
          "cannot parse signature file: " + PatFiles[I].string() + ": " +
              llvm::toString(std::move(Error)),
          llvm::inconvertibleErrorCode());
    for (const PatternChunk &Chunk : FileChunks)
      Count += Chunk.Modules.size();
  }

  std::vector<StoredModule> NewModules;
  NewModules.reserve(Count);
  std::vector<SigSource> NewSources;
  NewSources.reserve(PatFiles.size());
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    SigSource Source;
    Source.Path = PatFiles[I].string();
    Source.LibraryName = libraryName(PatFiles[I]);
    Source.ModuleStart = NewModules.size();
    if (Cached[I]) {
      NewModules.insert(NewModules.end(), Cached[I]->Modules.begin(),
                        Cached[I]->Modules.end());
      Source.Mapping = std::move(Cached[I]->Mapping);
      Source.Names.push_back(std::move(Cached[I]->Names));
    } else {
      Source.Bytes = std::move(Bytes[I]);
      for (size_t C = FirstChunk[I]; C < FirstChunk[I + 1]; ++C) {
        NewModules.insert(NewModules.end(), Chunks[C].Modules.begin(),
                          Chunks[C].Modules.end());
        Source.Names.push_back(std::move(Chunks[C].Names));
      }
    }
    Source.ModuleCount = NewModules.size() - Source.ModuleStart;
    NewSources.push_back(std::move(Source));
  }
  Modules.swap(NewModules);
  LoadedFiles.swap(NewSources);
  Index.reset();
  clearMatches();

  // What was parsed is kept for the next load, once the whole batch has
  // loaded.
  if (Cache)
    neverd::parallelForEach(LoadedFiles.size(), [&](auto Claim, size_t Total) {
      for (size_t I = Claim(); I < Total; I = Claim())
        if (Identities[I] && !LoadedFiles[I].Mapping)
          Cache->store(
              PatFiles[I], *Identities[I],
              llvm::ArrayRef(Modules).slice(LoadedFiles[I].ModuleStart,
                                            LoadedFiles[I].ModuleCount));
    });
  return llvm::Error::success();
}

void SignatureDB::apply(const BinaryImage &Img,
                        const std::vector<uint64_t> &FuncEntries) {
  clearMatches();
  if (Modules.empty() || FuncEntries.empty())
    return;

  const SignatureMatcher::HashIndex &Idx = index();

  // Collect executable segment data for matching.
  std::vector<SignatureMatcher::Hit> Hits;
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    for (const SignatureMatcher::Hit &Hit :
         SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                           Seg.VA, FuncEntries, Modules, Idx)) {
      Hits.push_back(Hit);
      const uint64_t Addr = Hit.Address;
      const size_t ModIdx = Hit.Module;
      const StoredModule &Mod = Modules[ModIdx];
      // The names a module gives one offset are one routine's aliases, so
      // each offset makes one match.
      std::map<uint32_t, std::vector<std::string_view>> ByOffset;
      for (const StoredName &Ref : Mod.publicNames())
        ByOffset[Ref.Offset].push_back(Ref.Name);
      for (auto &[Offset, Names] : ByOffset) {
        if (Offset > std::numeric_limits<uint64_t>::max() - Addr)
          continue;
        std::sort(Names.begin(), Names.end(), preferredAliasOrder);
        Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
        SigMatch M;
        M.Address = Addr + Offset;
        M.Name = std::string(Names.front());
        for (auto It = std::next(Names.begin()); It != Names.end(); ++It)
          M.Aliases.emplace_back(*It);
        M.LibraryName = libraryNameOf(ModIdx);
        M.FuncLen = Mod.TotalLen;
        Matches.push_back(std::move(M));
        MatchModules.push_back(ModIdx);
      }
    }
  }
  checkReferences(Img, FuncEntries, std::move(Hits));
}

size_t SignatureDB::identifyPersonalityRoutines(BinaryImage &Img) {
  const std::vector<va_t> Candidates = collectUnnamedPersonalityRoutines(Img);
  if (Candidates.empty() || Modules.empty())
    return 0;

  // One proposal per address, and the addresses two modules disagree about
  // recorded so they can be dropped: a routine that two signatures name
  // differently is a routine neither of them has identified.
  std::map<uint64_t, SigMatch> Proposed;
  std::set<uint64_t> Disputed;
  const SignatureMatcher::HashIndex &Idx = index();

  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    for (const SignatureMatcher::Hit &Hit :
         SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                           Seg.VA, Candidates, Modules, Idx)) {
      const StoredModule &Mod = Modules[Hit.Module];
      // Whole-function agreement is the main gate, but on its own it would
      // also be satisfied by a short pattern that is mostly wildcards.
      if (!SignatureMatcher::isFullyVerified(Mod) ||
          SignatureMatcher::fixedByteCount(Mod) <
              SignatureMatcher::MinStatedBytes)
        continue;

      // Of the personality routines the module names at its start -- aliases
      // of one routine when there are several -- the preferred.  A name at a
      // non-zero offset belongs to some other function the module also
      // describes, not to the routine being identified.
      std::optional<std::string_view> Chosen;
      for (const StoredName &Ref : Mod.publicNames()) {
        if (Ref.Offset != 0)
          continue;
        const ExceptionPersonality P = classifyPersonalityName(Ref.Name);
        if (P == ExceptionPersonality::None ||
            P == ExceptionPersonality::Unknown)
          continue;
        if (!Chosen || preferredAliasOrder(Ref.Name, *Chosen))
          Chosen = Ref.Name;
      }
      if (Chosen) {
        SigMatch M;
        M.Address = Hit.Address;
        M.Name = std::string(*Chosen);
        M.LibraryName = libraryNameOf(Hit.Module);
        M.FuncLen = Mod.TotalLen;
        auto [It, Fresh] = Proposed.emplace(Hit.Address, std::move(M));
        if (!Fresh && It->second.Name != *Chosen)
          Disputed.insert(Hit.Address);
      }
    }
  }

  size_t Named = 0;
  for (const auto &[Addr, Match] : Proposed) {
    if (Disputed.count(Addr))
      continue;
    if (!adoptPersonalityRoutineName(Img, Addr, Match.Name))
      continue;
    Matches.push_back(Match);
    MatchModules.push_back(NoModule);
    ++Named;
  }

  // The adopted names are in the symbol table now, and that is what the
  // image-wide detection reads.  A stripped image that had nothing to go on
  // may well have something now, so ask again -- but only when the earlier
  // answer was that there was no answer, because a runtime already proven
  // from sections and banners is better evidenced than one personality name.
  if (Named != 0 &&
      Img.ExceptionMetadata.Runtime.Runtime == SourceLanguageRuntime::Unknown)
    Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);

  return Named;
}

const SigMatch *SignatureDB::findMatch(uint64_t Addr) const {
  for (const auto &M : Matches) {
    if (M.Address == Addr)
      return &M;
  }
  return nullptr;
}

std::optional<SignatureDB::SettledRoutine>
SignatureDB::settle(llvm::ArrayRef<const SigMatch *> Proposals) {
  // Each match's names for its address: its name and its aliases, sorted.
  auto NamesOf = [](const SigMatch &M,
                    llvm::SmallVectorImpl<std::string_view> &Names) {
    Names.clear();
    Names.push_back(M.Name);
    for (const std::string &Alias : M.Aliases)
      Names.push_back(Alias);
    llvm::sort(Names);
    Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  };
  // Matches agree when a name is in every one of them; the routine then
  // takes the preferred such name, and has every name any of them gives it.
  auto Agree = [&](llvm::ArrayRef<const SigMatch *> Agreeing)
      -> std::optional<SettledRoutine> {
    if (Agreeing.empty())
      return std::nullopt;
    SettledRoutine Routine;
    llvm::SmallVector<std::string_view, 4> Shared, Names, Merged;
    NamesOf(*Agreeing.front(), Shared);
    Routine.Names = Shared;
    for (const SigMatch *M : Agreeing.drop_front()) {
      NamesOf(*M, Names);
      Merged.clear();
      std::set_intersection(Shared.begin(), Shared.end(), Names.begin(),
                            Names.end(), std::back_inserter(Merged));
      Shared.swap(Merged);
      Merged.clear();
      std::set_union(Routine.Names.begin(), Routine.Names.end(), Names.begin(),
                     Names.end(), std::back_inserter(Merged));
      Routine.Names.swap(Merged);
    }
    if (Shared.empty())
      return std::nullopt;
    Routine.Name =
        *std::min_element(Shared.begin(), Shared.end(), preferredAliasOrder);
    return Routine;
  };

  std::optional<SettledRoutine> Routine = Agree(Proposals);
  if (!Routine) {
    llvm::SmallVector<const SigMatch *, 4> Confirmed;
    for (const SigMatch *M : Proposals)
      if (M->Confirmed)
        Confirmed.push_back(M);
    Routine = Agree(Confirmed);
  }
  return Routine;
}

std::unordered_map<uint64_t, SignatureDB::SettledRoutine>
SignatureDB::settleRoutines() const {
  std::unordered_map<uint64_t, std::vector<const SigMatch *>> Proposed;
  for (const SigMatch &M : Matches)
    Proposed[M.Address].push_back(&M);
  std::unordered_map<uint64_t, SettledRoutine> Settled;
  for (const auto &[Address, Proposals] : Proposed)
    if (std::optional<SettledRoutine> Routine = settle(Proposals))
      Settled.emplace(Address, std::move(*Routine));
  return Settled;
}

std::unordered_map<uint64_t, std::string> SignatureDB::buildNameMap() const {
  std::unordered_map<uint64_t, std::string> Map;
  for (const auto &[Address, Routine] : settleRoutines())
    Map.emplace(Address, std::string(Routine.Name));
  return Map;
}

void SignatureDB::clear() {
  Modules.clear();
  LoadedFiles.clear();
  Index.reset();
  clearMatches();
}

void SignatureDB::clearMatches() {
  Matches.clear();
  MatchModules.clear();
}

namespace {

enum class ReferenceVerdict { Unknown, Confirmed, Contradicted };

#define NEVERD_BRANCH_FORM(Name, Mask, Match)                                  \
  [[maybe_unused]] constexpr InstructionForm Name{Mask, Match};
#define NEVERD_BRANCH_FIELD(Name, Low, Width)                                  \
  [[maybe_unused]] constexpr BitField Name{Low, Width};
#define NEVERD_BRANCH_VALUE(Name, Value)                                       \
  [[maybe_unused]] constexpr uint32_t Name = Value;
#include "neverd/support/BranchEncoding.def"

#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  [[maybe_unused]] constexpr uint32_t Name = Value;
#include "neverd/sigs/PatternSyntax.def"

#define NEVERD_SIGS_SYNTAX_CHAR(Name, Value)                                   \
  [[maybe_unused]] constexpr char Name = Value;
#include "neverd/sigs/LinkerSyntax.def"

#define NEVERD_SIGS_REFERENCE_LIMIT(Name, Value) constexpr size_t Name = Value;
#include "ReferenceCheckLimits.def"

uint64_t wrapToImage(const BinaryImage &Img, uint64_t Address) {
  return Img.is64Bit() ? Address : static_cast<uint32_t>(Address);
}

/// The word at \p Address, when the image holds it.
std::optional<uint32_t> wordAt(const BinaryImage &Img, uint64_t Address) {
  const uint8_t *Bytes = Img.readVA(Address, sizeof(uint32_t));
  if (!Bytes)
    return std::nullopt;
  return readLE<uint32_t>(Bytes);
}

/// The halfword at \p Address, when the image holds it.
std::optional<uint16_t> halfwordAt(const BinaryImage &Img, uint64_t Address) {
  const uint8_t *Bytes = Img.readVA(Address, sizeof(uint16_t));
  if (!Bytes)
    return std::nullopt;
  return readLE<uint16_t>(Bytes);
}

/// Where a direct branch goes, and on 32-bit ARM whether it enters its
/// target in Thumb state.
struct Branch {
  uint64_t Target = 0;
  bool Thumb = false;
};

/// The direct branch of one of \p Forms at \p Address, in \p Mode, when the
/// image holds one.  A 32-bit image's addresses wrap at 32 bits; an A64
/// program counter is 64 bits wide whatever the image's pointers are.
std::optional<Branch> directBranch(const BinaryImage &Img, uint64_t Address,
                                   InstructionMode Mode,
                                   DirectBranchForms Forms) {
  const size_t Length = getDirectBranchLength(Img.Arch);
  const uint8_t *Insn = Img.readVA(Address, Length);
  if (!Insn)
    return std::nullopt;
  const std::optional<DirectBranch> Decoded =
      decodeDirectBranch(Img.Arch, Mode, Insn, Length, Address, Forms);
  if (!Decoded)
    return std::nullopt;
  const uint64_t Target = Decoded->wrappingTarget();
  return Branch{Img.Arch == Arch::AArch64 ? Target : wrapToImage(Img, Target),
                Decoded->TargetIsThumb};
}

/// The branches a reference names: direct jumps and calls, and on 32-bit ARM
/// the calls that switch state too.  An A32 reference also names a branch
/// under a condition.
DirectBranchForms referenceForms(Arch A, InstructionMode Mode) {
  DirectBranchForms Forms;
  Forms.Jumps = true;
  Forms.Calls = true;
  Forms.ExchangingCalls = A == Arch::ARM;
  Forms.Conditional = A == Arch::ARM && Mode == InstructionMode::ARM;
  return Forms;
}

/// The branch a thunk that only jumps on starts with: an unconditional jump.
DirectBranchForms thunkForms() {
  DirectBranchForms Forms;
  Forms.Jumps = true;
  return Forms;
}

/// Where the direct branch a reference at \p Offset of the routine at
/// \p Start describes goes, when the image holds that branch; see
/// PatternModule::References for the offsets.
std::optional<Branch> branchTarget(const BinaryImage &Img, uint64_t Start,
                                   uint32_t Offset) {
  const uint64_t Site = Start + Offset;
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64:
    // The reference states the rel32 field, which follows the opcode.
    if (Site < x86::kRel32DispOffset)
      return std::nullopt;
    return directBranch(Img, Site - x86::kRel32DispOffset,
                        InstructionMode::Default,
                        referenceForms(Img.Arch, InstructionMode::Default));
  case Arch::AArch64:
    // B and BL; a veneer or anything else is not the branch the library had.
    return directBranch(Img, Site, InstructionMode::Default,
                        referenceForms(Img.Arch, InstructionMode::Default));
  case Arch::ARM:
    if (Offset & ArmStateReferenceMark)
      return directBranch(Img, Site - ArmStateReferenceMark,
                          InstructionMode::ARM,
                          referenceForms(Img.Arch, InstructionMode::ARM));
    return directBranch(Img, Site, InstructionMode::Thumb,
                        referenceForms(Img.Arch, InstructionMode::Thumb));
  default:
    return std::nullopt;
  }
}

/// Where a 32-bit ARM code address \p Address goes, in the state its low bit
/// says.
Branch interworkingTarget(const BinaryImage &Img, uint64_t Address) {
  return Branch{wrapToImage(Img, Address & ~uint64_t(ThumbStateBit)),
                (Address & ThumbStateBit) != 0};
}

/// The immediate an ARM-state MOVW (or, when \p Top, MOVT) writes to ip.
std::optional<BitString> armMoveToIP(uint32_t Word, bool Top) {
  if (!(Top ? ArmMoveTopToIP : ArmMoveWideToIP).matches(Word))
    return std::nullopt;
  return BitString().append(ArmMoveImm4, Word).append(ArmMoveImm12, Word);
}

/// The immediate a Thumb-2 MOVW (or, when \p Top, MOVT) at \p Address
/// writes to ip.
std::optional<BitString> thumbMoveToIP(const BinaryImage &Img, uint64_t Address,
                                       bool Top) {
  const std::optional<uint16_t> First = halfwordAt(Img, Address);
  const std::optional<uint16_t> Second =
      halfwordAt(Img, Address + ThumbHalfwordBytes);
  if (!First || !Second ||
      !(Top ? ThumbMoveTopToIPHigh : ThumbMoveWideToIPHigh).matches(*First) ||
      !ThumbMoveToIPLow.matches(*Second))
    return std::nullopt;
  return BitString()
      .append(ThumbMoveImm4, *First)
      .append(ThumbMoveI, *First)
      .append(ThumbMoveImm3, *Second)
      .append(ThumbMoveImm8, *Second);
}

/// The routine an ARM-state linker thunk at \p Address forwards to: an
/// unconditional B, `ldr pc, [pc, #-4]` and the address after it (ARMv5 and
/// GNU long branches), or lld's `movw ip; movt ip; [add ip, ip, pc;] bx ip`.
std::optional<Branch> armThunkTarget(const BinaryImage &Img, uint64_t Address) {
  if (std::optional<Branch> Jump =
          directBranch(Img, Address, InstructionMode::ARM, thunkForms()))
    return Jump;
  const uint64_t SecondAddress = Address + ArmInstructionBytes;
  const std::optional<uint32_t> First = wordAt(Img, Address),
                                Second = wordAt(Img, SecondAddress);
  if (!First || !Second)
    return std::nullopt;
  if (ArmLoadPCFromNextWord.matches(*First))
    return interworkingTarget(Img, *Second);
  const std::optional<BitString> Low = armMoveToIP(*First, /*Top=*/false);
  const std::optional<BitString> High =
      Low ? armMoveToIP(*Second, /*Top=*/true) : std::nullopt;
  const uint64_t ThirdAddress = SecondAddress + ArmInstructionBytes;
  const std::optional<uint32_t> Third =
      High ? wordAt(Img, ThirdAddress) : std::nullopt;
  if (!Third)
    return std::nullopt;
  const uint64_t Value = BitString(*High).append(*Low).zeroExtended();
  if (ArmBranchExchangeIP.matches(*Third))
    return interworkingTarget(Img, Value);
  // `add ip, ip, pc` reads pc eight bytes past itself.
  if (ArmAddIPIPPC.matches(*Third)) {
    const std::optional<uint32_t> Fourth =
        wordAt(Img, ThirdAddress + ArmInstructionBytes);
    if (Fourth && ArmBranchExchangeIP.matches(*Fourth))
      return interworkingTarget(Img, Value + ThirdAddress + ArmPCOffset);
  }
  return std::nullopt;
}

/// The routine a Thumb linker thunk at \p Address forwards to: a B.W, or
/// lld's `movw ip; movt ip; [add ip, pc;] bx ip`.
std::optional<Branch> thumbThunkTarget(const BinaryImage &Img,
                                       uint64_t Address) {
  if (std::optional<Branch> Jump =
          directBranch(Img, Address, InstructionMode::Thumb, thunkForms()))
    return Jump;
  const uint64_t SecondAddress = Address + ThumbWideInstructionBytes;
  const std::optional<BitString> Low =
      thumbMoveToIP(Img, Address, /*Top=*/false);
  const std::optional<BitString> High =
      Low ? thumbMoveToIP(Img, SecondAddress, /*Top=*/true) : std::nullopt;
  if (!High)
    return std::nullopt;
  const uint64_t Value = BitString(*High).append(*Low).zeroExtended();
  const uint64_t ThirdAddress = SecondAddress + ThumbWideInstructionBytes;
  const std::optional<uint16_t> Third = halfwordAt(Img, ThirdAddress);
  if (!Third)
    return std::nullopt;
  if (ThumbBranchExchangeIP.matches(*Third))
    return interworkingTarget(Img, Value);
  // `add ip, pc` reads pc four bytes past itself.
  if (ThumbAddIPPC.matches(*Third)) {
    const std::optional<uint16_t> Fourth =
        halfwordAt(Img, ThirdAddress + ThumbHalfwordBytes);
    if (Fourth && ThumbBranchExchangeIP.matches(*Fourth))
      return interworkingTarget(Img, Value + ThirdAddress + ThumbPCOffset);
  }
  return std::nullopt;
}

/// The routine an AArch64 linker thunk at \p Address forwards to: a B,
/// `adrp x16; add x16, x16, #lo12; br x16`, or `ldr x16, #8; br x16` and the
/// address after them.
std::optional<Branch> aarch64ThunkTarget(const BinaryImage &Img,
                                         uint64_t Address) {
  if (std::optional<Branch> Jump =
          directBranch(Img, Address, InstructionMode::Default, thunkForms()))
    return Jump;
  const std::optional<uint32_t> First = wordAt(Img, Address);
  if (!First)
    return std::nullopt;
  const uint64_t SecondAddress = Address + A64InstructionBytes;
  const uint64_t ThirdAddress = SecondAddress + A64InstructionBytes;
  const std::optional<uint32_t> Second = wordAt(Img, SecondAddress),
                                Third = wordAt(Img, ThirdAddress);
  if (!Second || !Third)
    return std::nullopt;
  if (A64PageAddressX16.matches(*First) && A64AddX16X16.matches(*Second) &&
      A64BranchRegisterX16.matches(*Third)) {
    const int64_t Pages = BitString()
                              .append(A64PageImmHi, *First)
                              .append(A64PageImmLo, *First)
                              .signExtended();
    const uint64_t Page = llvm::alignDown(Address, A64PageBytes) +
                          static_cast<uint64_t>(Pages * A64PageBytes);
    return Branch{Page + A64AddImm12.extract(*Second)};
  }
  if (A64LoadX16FromThunkEnd.matches(*First) &&
      A64BranchRegisterX16.matches(*Second)) {
    const uint64_t Literal =
        Address + A64LoadLiteralOffset.extract(*First) * A64InstructionBytes;
    if (const uint8_t *Bytes = Img.readVA(Literal, sizeof(uint64_t)))
      return Branch{readLE<uint64_t>(Bytes)};
  }
  return std::nullopt;
}

/// Where the thunk the branch \p To enters jumps to, when all the thunk does
/// is jump on: an incremental-linking thunk, a branch island, or a linker's
/// long-branch or interworking thunk.  On 32-bit ARM the thunk runs in the
/// state \p To enters it in.
std::optional<Branch> thunkTarget(const BinaryImage &Img, const Branch &To) {
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64:
    return directBranch(Img, To.Target, InstructionMode::Default, thunkForms());
  case Arch::AArch64:
    return aarch64ThunkTarget(Img, To.Target);
  case Arch::ARM:
    return To.Thumb ? thumbThunkTarget(Img, To.Target)
                    : armThunkTarget(Img, To.Target);
  default:
    return std::nullopt;
  }
}

} // namespace

void SignatureDB::checkReferences(const BinaryImage &Img,
                                  llvm::ArrayRef<uint64_t> Entries,
                                  std::vector<SignatureMatcher::Hit> Hits) {
  bool AnyReferences = false;
  for (size_t Module : MatchModules)
    AnyReferences |= Module != NoModule && Modules[Module].ReferenceCount != 0;
  if (!AnyReferences)
    return;
  auto ReferencesOf = [&](size_t I) {
    return MatchModules[I] == NoModule ? llvm::ArrayRef<StoredName>()
                                       : Modules[MatchModules[I]].references();
  };

  // Where each reference of each match branches: the branch's target and,
  // when that is a routine that only jumps on, the routine it reaches.  The
  // references at one offset are one branch, which reaches one of the
  // routines they name: a COFF link resolves a symbol no object defines to
  // its alternate name.  A branch the image does not hold at its site is
  // none of them, and leaves its match unconfirmed.  Match I's are
  // Sites[FirstSite[I]] up to Sites[FirstSite[I + 1]], of Branches[I].
  struct Site {
    llvm::SmallVector<std::string_view, 1> Names;
    uint64_t Target = 0;
    std::optional<uint64_t> Onward;
  };
  std::vector<Site> Sites;
  std::vector<size_t> FirstSite(Matches.size() + 1, 0);
  std::vector<uint32_t> Branches(Matches.size(), 0);
  for (size_t I = 0; I < Matches.size(); ++I) {
    FirstSite[I] = Sites.size();
    llvm::SmallVector<StoredName, 8> References(ReferencesOf(I).begin(),
                                                ReferencesOf(I).end());
    if (References.empty())
      continue;
    llvm::stable_sort(References, [](const StoredName &A, const StoredName &B) {
      return A.Offset < B.Offset;
    });
    // Every public name of a module shares its references; the module's
    // start is the match address less the name's offset.
    uint64_t Start = Matches[I].Address;
    for (const StoredName &Name : Modules[MatchModules[I]].publicNames())
      if (Name.Name == Matches[I].Name) {
        Start = Matches[I].Address - Name.Offset;
        break;
      }
    if (Img.Arch == Arch::ARM)
      Start &= ~uint64_t(ThumbStateBit);
    for (size_t R = 0; R < References.size();) {
      size_t End = R + 1;
      while (End < References.size() &&
             References[End].Offset == References[R].Offset)
        ++End;
      ++Branches[I];
      if (const std::optional<Branch> To =
              branchTarget(Img, Start, References[R].Offset)) {
        Site Where;
        for (size_t Alternative = R; Alternative < End; ++Alternative)
          Where.Names.push_back(References[Alternative].Name);
        Where.Target = To->Target;
        if (const std::optional<Branch> Next = thunkTarget(Img, *To))
          Where.Onward = Next->Target;
        Sites.push_back(std::move(Where));
      }
      R = End;
    }
  }
  FirstSite[Matches.size()] = Sites.size();

  // The matches whose references branch to each address, and the matches at
  // each address, as sorted (address, match) pairs.
  using Entry = std::pair<uint64_t, size_t>;
  std::vector<Entry> Calling, At;
  for (size_t I = 0; I < Matches.size(); ++I) {
    At.emplace_back(Matches[I].Address, I);
    for (size_t S = FirstSite[I]; S < FirstSite[I + 1]; ++S) {
      Calling.emplace_back(Sites[S].Target, I);
      if (Sites[S].Onward)
        Calling.emplace_back(*Sites[S].Onward, I);
    }
  }
  llvm::sort(Calling);
  Calling.erase(std::unique(Calling.begin(), Calling.end()), Calling.end());
  llvm::sort(At);
  auto Range = [](const std::vector<Entry> &Pairs, uint64_t Address) {
    const auto Begin =
        std::lower_bound(Pairs.begin(), Pairs.end(), Entry{Address, 0});
    auto End = Begin;
    while (End != Pairs.end() && End->first == Address)
      ++End;
    return llvm::make_range(Begin, End);
  };

  // The modules whose patterns match where a reference branches, to confirm
  // a reference by the named routine's own pattern: at an entry, the ones
  // that matched there; at any other target, the ones the index finds.
  std::vector<uint64_t> Tried(Entries.begin(), Entries.end());
  llvm::sort(Tried);
  std::vector<uint64_t> Untried;
  for (size_t C = 0; C < Calling.size(); ++C)
    if ((C == 0 || Calling[C].first != Calling[C - 1].first) &&
        !std::binary_search(Tried.begin(), Tried.end(), Calling[C].first))
      Untried.push_back(Calling[C].first);
  if (!Untried.empty())
    for (const Segment &Seg : Img.Segments)
      if (Seg.isExecutable() && !Seg.Data.empty())
        for (const SignatureMatcher::Hit &Hit :
             SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                               Seg.VA, Untried, Modules,
                                               index()))
          Hits.push_back(Hit);
  std::vector<Entry> PatternsAt;
  PatternsAt.reserve(Hits.size());
  for (const SignatureMatcher::Hit &Hit : Hits)
    PatternsAt.emplace_back(Hit.Address, Hit.Module);
  llvm::sort(PatternsAt);
  auto PatternAt = [&](std::string_view Name, uint64_t Address) {
    for (const Entry &Hit : Range(PatternsAt, Address))
      for (const StoredName &Public : Modules[Hit.second].publicNames())
        if (Public.Offset == 0 && Public.Name == Name)
          return true;
    return false;
  };

  // What the matches settle where a reference branches, which is what a
  // reference is checked against: an address two matches name differently
  // names nothing.  At first that is what the bytes alone settle.  A Thumb
  // routine may be entered with the interworking bit set.
  std::unordered_map<uint64_t, SettledRoutine> Settled;
  std::vector<bool> Dropped(Matches.size(), false);
  auto SettleAt = [&](uint64_t Address) {
    std::vector<const SigMatch *> Proposals;
    for (const Entry &Match : Range(At, Address))
      if (!Dropped[Match.second])
        Proposals.push_back(&Matches[Match.second]);
    return settle(Proposals);
  };
  for (size_t C = 0; C < Calling.size(); ++C) {
    if (C != 0 && Calling[C].first == Calling[C - 1].first)
      continue;
    const uint64_t Target = Calling[C].first;
    for (uint64_t Address :
         {Target, Img.Arch == Arch::ARM ? Target | ThumbStateBit : Target})
      if (!Settled.count(Address))
        if (std::optional<SettledRoutine> Routine = SettleAt(Address))
          Settled.emplace(Address, std::move(*Routine));
  }
  auto SettledAt = [&](uint64_t Target) {
    auto It = Settled.find(Target);
    if (It == Settled.end() && Img.Arch == Arch::ARM)
      It = Settled.find(Target | ThumbStateBit);
    return It;
  };

  auto Judge = [&](std::string_view Name, uint64_t Target) {
    if (const Import *Imp = Img.findImportStubAt(Target)) {
      // An ELF import is the very symbol the library called, named without
      // the version a `name@VERSION` reference states, so its stub settles
      // the call either way.  A COFF import thunk is named after the import,
      // not after the decorated symbol the library called; it settles
      // nothing.
      if (!Img.isELF())
        return ReferenceVerdict::Unknown;
      return Imp->Name == Name.substr(0, Name.find(ELFVersionSeparator))
                 ? ReferenceVerdict::Confirmed
                 : ReferenceVerdict::Contradicted;
    }
    // A library calls a routine by whichever of its names it uses, so any
    // name the routine settled with confirms the call.
    if (const auto It = SettledAt(Target); It != Settled.end())
      return It->second.hasName(Name) ? ReferenceVerdict::Confirmed
                                      : ReferenceVerdict::Contradicted;
    // A routine the image replaced (operator new, say) does not match the
    // library's pattern and is still the routine called, so a pattern that
    // does not match contradicts nothing.
    return PatternAt(Name, Target) ? ReferenceVerdict::Confirmed
                                   : ReferenceVerdict::Unknown;
  };

  // Whether match \p I is contradicted, and whether every one of its
  // branches is confirmed.
  enum class Outcome : uint8_t { Unconfirmed, Confirmed, Contradicted };
  auto Check = [&](size_t I) {
    size_t Confirmed = 0;
    for (size_t S = FirstSite[I]; S < FirstSite[I + 1]; ++S) {
      const Site &Where = Sites[S];
      // Any routine the branch may reach confirms it; it contradicts the
      // match only when the routine it reaches is none of them.
      ReferenceVerdict Verdict = ReferenceVerdict::Contradicted;
      for (std::string_view Name : Where.Names) {
        const ReferenceVerdict One = Judge(Name, Where.Target);
        if (One == ReferenceVerdict::Confirmed) {
          Verdict = One;
          break;
        }
        if (One == ReferenceVerdict::Unknown)
          Verdict = One;
      }
      // A routine that only jumps on is a thunk, or a routine that
      // tail-calls another -- `free` that jumps to `_free_base`, `operator
      // delete` to `free` -- and the bytes cannot tell which.  So the
      // routine it reaches confirms the reference when it is the one named,
      // and otherwise contradicts nothing.
      if (Verdict == ReferenceVerdict::Unknown && Where.Onward &&
          llvm::any_of(Where.Names, [&](std::string_view Name) {
            return Judge(Name, *Where.Onward) == ReferenceVerdict::Confirmed;
          }))
        Verdict = ReferenceVerdict::Confirmed;
      if (Verdict == ReferenceVerdict::Contradicted)
        return Outcome::Contradicted;
      Confirmed += Verdict == ReferenceVerdict::Confirmed;
    }
    return Confirmed != 0 && Confirmed == Branches[I] ? Outcome::Confirmed
                                                      : Outcome::Unconfirmed;
  };

  // A routine the references name is one its callers' references can be
  // checked against: a leaf that calls an import tells two same-byte
  // instantiations apart, and then so does every routine that calls one of
  // them.  So the check repeats, each round against what the last one
  // settled, until a round settles nothing new.  A match's verdict changes
  // only with what its references' targets settle, so a round checks only
  // the matches that call an address the last one settled differently.
  // Matches are only ever dropped, and the rounds are bounded all the same.
  // The first round checks every match with references; a round with many
  // to check checks them in parallel, against what the last one settled.
  std::vector<size_t> Work;
  for (size_t I = 0; I < Matches.size(); ++I)
    if (!ReferencesOf(I).empty())
      Work.push_back(I);
  for (size_t Round = 0; Round < MaxReferenceRounds && !Work.empty(); ++Round) {
    std::vector<Outcome> Outcomes(Work.size());
    if (Work.size() >= ParallelReferenceMatches) {
      const size_t Blocks =
          llvm::divideCeil(Work.size(), ReferenceMatchesPerBlock);
      neverd::parallelForEach(Blocks, [&](auto Claim, size_t Total) {
        for (size_t B = Claim(); B < Total; B = Claim()) {
          const size_t End =
              std::min(Work.size(), (B + 1) * ReferenceMatchesPerBlock);
          for (size_t W = B * ReferenceMatchesPerBlock; W < End; ++W)
            Outcomes[W] = Check(Work[W]);
        }
      });
    } else {
      for (size_t W = 0; W < Work.size(); ++W)
        Outcomes[W] = Check(Work[W]);
    }

    std::set<uint64_t> Touched;
    for (size_t W = 0; W < Work.size(); ++W) {
      const size_t I = Work[W];
      if (Outcomes[W] == Outcome::Contradicted)
        Dropped[I] = true;
      else if ((Outcomes[W] == Outcome::Confirmed) != Matches[I].Confirmed)
        Matches[I].Confirmed = Outcomes[W] == Outcome::Confirmed;
      else
        continue;
      Touched.insert(Matches[I].Address);
    }

    std::set<size_t> Next;
    for (uint64_t Address : Touched) {
      // SettledAt reads a Thumb routine's settlement for the address without
      // its interworking bit too; an address no reference reads is not kept.
      const uint64_t Even =
          Img.Arch == Arch::ARM ? Address & ~uint64_t(ThumbStateBit) : Address;
      if (Range(Calling, Address).empty() && Range(Calling, Even).empty())
        continue;
      std::optional<SettledRoutine> Routine = SettleAt(Address);
      const auto Old = Settled.find(Address);
      if (Routine ? Old != Settled.end() && Old->second == *Routine
                  : Old == Settled.end())
        continue;
      if (Routine)
        Settled.insert_or_assign(Address, std::move(*Routine));
      else
        Settled.erase(Old);
      for (uint64_t Callee : {Address, Even})
        for (const Entry &Caller : Range(Calling, Callee))
          if (!Dropped[Caller.second])
            Next.insert(Caller.second);
    }
    Work.assign(Next.begin(), Next.end());
  }

  std::vector<SigMatch> Kept;
  std::vector<size_t> KeptModules;
  Kept.reserve(Matches.size());
  KeptModules.reserve(Matches.size());
  for (size_t I = 0; I < Matches.size(); ++I)
    if (!Dropped[I]) {
      Kept.push_back(std::move(Matches[I]));
      KeptModules.push_back(MatchModules[I]);
    }
  Matches = std::move(Kept);
  MatchModules = std::move(KeptModules);
}
