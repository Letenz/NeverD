//===- SignatureDB.cpp - Signature database manager -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sigs/SignatureDB.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/sigs/PatternParser.h"
#include "neverd/sigs/SignatureMatcher.h"
#include "neverd/support/BinaryEncoding.h"

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

} // namespace

void SignatureDB::commitSource(std::vector<PatternChunk> &&Chunks,
                               std::unique_ptr<uint8_t[]> Bytes,
                               const std::string &LibName,
                               const std::string &FilePath) {
  std::vector<StoredModule> Mods;
  std::vector<PatternNames> Names;
  for (PatternChunk &Chunk : Chunks) {
    Mods.insert(Mods.end(), Chunk.Modules.begin(), Chunk.Modules.end());
    Names.push_back(std::move(Chunk.Names));
  }

  auto Existing = std::find_if(
      LoadedFiles.begin(), LoadedFiles.end(),
      [&](const SigSource &Source) { return Source.Path == FilePath; });
  if (Existing != LoadedFiles.end()) {
    const size_t Start = Existing->ModuleStart;
    Modules.erase(Modules.begin() + Start,
                  Modules.begin() + Start + Existing->ModuleCount);
    Modules.insert(Modules.begin() + Start, Mods.begin(), Mods.end());
    Existing->LibraryName = LibName;
    Existing->ModuleCount = Mods.size();
    Existing->Bytes = std::move(Bytes);
    Existing->Names = std::move(Names);

    size_t ModuleStart = 0;
    for (SigSource &Source : LoadedFiles) {
      Source.ModuleStart = ModuleStart;
      ModuleStart += Source.ModuleCount;
    }
    Index.reset();
    clearMatches();
    return;
  }

  SigSource Src;
  Src.Path = FilePath;
  Src.LibraryName = LibName;
  Src.ModuleStart = Modules.size();
  Src.ModuleCount = Mods.size();
  Src.Bytes = std::move(Bytes);
  Src.Names = std::move(Names);
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

  if (Ext == ".pat") {
    auto BufferOrErr = llvm::MemoryBuffer::getFile(
        Path.string(), /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!BufferOrErr)
      return llvm::make_error<llvm::StringError>(
          "cannot open pattern file: " + Path.string(),
          llvm::inconvertibleErrorCode());
    auto SourceOrErr = parseSource((*BufferOrErr)->getBuffer());
    if (!SourceOrErr)
      return SourceOrErr.takeError();
    commitSource(std::move(SourceOrErr->Chunks), std::move(SourceOrErr->Bytes),
                 libraryName(Path), Path.string());
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
  commitSource(std::move(SourceOrErr->Chunks), std::move(SourceOrErr->Bytes),
               LibraryName.str(), LibraryName.str());
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
    if (IsRegular && It->path().extension() == ".pat")
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
  const size_t Dot = Stem.rfind(".part");
  if (Dot == std::string::npos || Dot == 0)
    return Stem;
  const llvm::StringRef Number = llvm::StringRef(Stem).drop_front(Dot + 5);
  unsigned Part = 0;
  // Part 1 is the file under the library's own name; a part number is
  // written without leading zeros.
  if (Number.empty() || Number.front() == '0' ||
      Number.getAsInteger(10, Part) || Part < 2)
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
    unsigned Year = 0;
    if (Stem.size() != 6 || llvm::StringRef(Stem).take_front(2) != "vs" ||
        llvm::StringRef(Stem).drop_front(2).getAsInteger(10, Year))
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

llvm::Error SignatureDB::loadDirectory(const std::filesystem::path &Dir) {
  auto PatFiles = listDirectory(Dir);
  if (!PatFiles)
    return PatFiles.takeError();
  return loadFiles(*PatFiles);
}

llvm::Error
SignatureDB::loadFiles(const std::vector<std::filesystem::path> &PatFiles) {
  // Every file is cut into chunks of whole lines, and one pool of workers
  // parses the chunks of all of them: one large file keeps every worker as
  // busy as many small ones do.  A parsed chunk no longer refers to its text,
  // so a file is let go as soon as its last chunk is done.
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Buffers(PatFiles.size());
  std::vector<std::unique_ptr<uint8_t[]>> Bytes(PatFiles.size());
  std::vector<PatternChunk> Chunks;
  std::vector<size_t> FirstChunk(PatFiles.size() + 1), FileOf;
  for (size_t I = 0; I < PatFiles.size(); ++I) {
    FirstChunk[I] = Chunks.size();
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
    Source.Bytes = std::move(Bytes[I]);
    for (size_t C = FirstChunk[I]; C < FirstChunk[I + 1]; ++C) {
      NewModules.insert(NewModules.end(), Chunks[C].Modules.begin(),
                        Chunks[C].Modules.end());
      Source.Names.push_back(std::move(Chunks[C].Names));
    }
    Source.ModuleCount = NewModules.size() - Source.ModuleStart;
    NewSources.push_back(std::move(Source));
  }
  Modules.swap(NewModules);
  LoadedFiles.swap(NewSources);
  Index.reset();
  clearMatches();
  return llvm::Error::success();
}

void SignatureDB::apply(const BinaryImage &Img,
                        const std::vector<uint64_t> &FuncEntries) {
  clearMatches();
  if (Modules.empty() || FuncEntries.empty())
    return;

  const SignatureMatcher::HashIndex &Idx = index();

  // Collect executable segment data for matching.
  for (const auto &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.empty())
      continue;

    for (const SignatureMatcher::Hit &Hit :
         SignatureMatcher::findAtAddresses(Seg.Data.data(), Seg.Data.size(),
                                           Seg.VA, FuncEntries, Modules, Idx)) {
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
  checkReferences(Img);
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

std::unordered_map<uint64_t, SignatureDB::SettledRoutine>
SignatureDB::settleRoutines() const {
  // Each match's names for its address: its name and its aliases.
  struct Proposal {
    std::set<std::string> Names;
    bool Confirmed = false;
  };
  std::unordered_map<uint64_t, std::vector<Proposal>> Proposed;
  for (const SigMatch &M : Matches) {
    Proposal P;
    P.Names.insert(M.Name);
    P.Names.insert(M.Aliases.begin(), M.Aliases.end());
    P.Confirmed = M.Confirmed;
    Proposed[M.Address].push_back(std::move(P));
  }

  // Proposals agree when a name is in every one of them; the routine then
  // takes the preferred such name, and has every name any of them gives it.
  auto Agree = [](const std::vector<const Proposal *> &Proposals)
      -> std::optional<SettledRoutine> {
    if (Proposals.empty())
      return std::nullopt;
    std::set<std::string> Shared = Proposals.front()->Names;
    SettledRoutine Routine;
    for (const Proposal *P : Proposals) {
      std::set<std::string> Next;
      std::set_intersection(Shared.begin(), Shared.end(), P->Names.begin(),
                            P->Names.end(), std::inserter(Next, Next.end()));
      Shared = std::move(Next);
      Routine.Names.insert(P->Names.begin(), P->Names.end());
    }
    if (Shared.empty())
      return std::nullopt;
    Routine.Name = *std::min_element(Shared.begin(), Shared.end(),
                                     [](const std::string &A,
                                        const std::string &B) {
                                       return preferredAliasOrder(A, B);
                                     });
    return Routine;
  };

  std::unordered_map<uint64_t, SettledRoutine> Settled;
  for (const auto &[Address, Proposals] : Proposed) {
    std::vector<const Proposal *> All, Confirmed;
    for (const Proposal &P : Proposals) {
      All.push_back(&P);
      if (P.Confirmed)
        Confirmed.push_back(&P);
    }
    std::optional<SettledRoutine> Routine = Agree(All);
    if (!Routine)
      Routine = Agree(Confirmed);
    if (Routine)
      Settled.emplace(Address, std::move(*Routine));
  }
  return Settled;
}

std::unordered_map<uint64_t, std::string> SignatureDB::buildNameMap() const {
  std::unordered_map<uint64_t, std::string> Map;
  for (auto &[Address, Routine] : settleRoutines())
    Map.emplace(Address, std::move(Routine.Name));
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

int64_t signExtend(uint64_t Value, unsigned Bits) {
  const uint64_t Sign = uint64_t(1) << (Bits - 1);
  return static_cast<int64_t>((Value ^ Sign) - Sign);
}

uint64_t wrapToImage(const BinaryImage &Img, uint64_t Address) {
  return Img.is64Bit() ? Address : Address & 0xFFFFFFFFu;
}

/// Where a direct branch goes, and on 32-bit ARM whether it enters its
/// target in Thumb state.
struct Branch {
  uint64_t Target = 0;
  bool Thumb = false;
};

/// The Thumb-2 B.W (T4), BL (T1) or BLX (T2) at \p Address, or only a B.W
/// when \p JumpOnly.
std::optional<Branch> thumbBranch(const BinaryImage &Img, uint64_t Address,
                                  bool JumpOnly) {
  const uint8_t *Insn = Img.readVA(Address, 4);
  if (!Insn)
    return std::nullopt;
  const uint16_t First = readLE<uint16_t>(Insn);
  const uint16_t Second = readLE<uint16_t>(Insn + 2);
  if ((First & 0xF800) != 0xF000)
    return std::nullopt;
  const unsigned Kind = Second & 0xD000;
  const bool Jump = Kind == 0x9000, Link = Kind == 0xD000,
             Exchange = Kind == 0xC000;
  if (!(Jump || (!JumpOnly && (Link || Exchange))))
    return std::nullopt;
  const uint64_t S = (First >> 10) & 1;
  const uint64_t I1 = ~(((Second >> 13) & 1) ^ S) & 1;
  const uint64_t I2 = ~(((Second >> 11) & 1) ^ S) & 1;
  const uint64_t Offset = (S << 24) | (I1 << 23) | (I2 << 22) |
                          (uint64_t(First & 0x3FF) << 12) |
                          (uint64_t(Second & 0x7FF) << 1);
  uint64_t Target = Address + 4 + static_cast<uint64_t>(signExtend(Offset, 25));
  if (Exchange)
    Target &= ~uint64_t(3);
  return Branch{wrapToImage(Img, Target), !Exchange};
}

/// The ARM-state B or BL, either of them conditional, or BLX (immediate) at
/// \p Address, or only an unconditional B when \p JumpOnly.
std::optional<Branch> armBranch(const BinaryImage &Img, uint64_t Address,
                                bool JumpOnly) {
  const uint8_t *Insn = Img.readVA(Address, 4);
  if (!Insn)
    return std::nullopt;
  const uint32_t Word = readLE<uint32_t>(Insn);
  if ((Word & 0x0E000000u) != 0x0A000000u)
    return std::nullopt;
  const bool Exchange = Word >> 28 == 0xF;
  // Bit 24 is BL's link bit, and BLX's halfword bit.
  const bool Bit24 = (Word >> 24) & 1;
  if (JumpOnly && (Exchange || Bit24 || Word >> 28 != 0xE))
    return std::nullopt;
  uint64_t Offset =
      static_cast<uint64_t>(signExtend(Word & 0x00FFFFFFu, 24) * 4);
  if (Exchange && Bit24)
    Offset += 2;
  return Branch{wrapToImage(Img, Address + 8 + Offset), Exchange};
}

/// Where the direct branch a reference at \p Offset of the routine at
/// \p Start describes goes, when the image holds that branch; see
/// PatternModule::References for the offsets.
std::optional<Branch> branchTarget(const BinaryImage &Img, uint64_t Start,
                                   uint32_t Offset) {
  const uint64_t Site = Start + Offset;
  switch (Img.Arch) {
  case Arch::X86:
  case Arch::X64: {
    if (Site == 0)
      return std::nullopt;
    const uint8_t *Insn = Img.readVA(Site - 1, 5);
    if (!Insn || (Insn[0] != 0xE8 && Insn[0] != 0xE9))
      return std::nullopt;
    const int64_t Disp = readLE<int32_t>(Insn + 1);
    return Branch{wrapToImage(Img, Site + 4 + static_cast<uint64_t>(Disp))};
  }
  case Arch::AArch64: {
    const uint8_t *Insn = Img.readVA(Site, 4);
    if (!Insn)
      return std::nullopt;
    const uint32_t Word = readLE<uint32_t>(Insn);
    // B and BL; a veneer or anything else is not the branch the library had.
    if ((Word & 0x7C000000u) != 0x14000000u)
      return std::nullopt;
    return Branch{
        Site + static_cast<uint64_t>(signExtend(Word & 0x03FFFFFFu, 26) * 4)};
  }
  case Arch::ARM:
    // An odd offset states an ARM-state branch one byte before it.
    if (Offset & 1)
      return armBranch(Img, Site - 1, /*JumpOnly=*/false);
    return thumbBranch(Img, Site, /*JumpOnly=*/false);
  default:
    return std::nullopt;
  }
}

/// Where a 32-bit ARM code address \p Address goes in the state its low bit
/// says: set for Thumb.
Branch interworkingTarget(const BinaryImage &Img, uint64_t Address) {
  return Branch{wrapToImage(Img, Address & ~uint64_t(1)), (Address & 1) != 0};
}

/// The immediate an ARM-state MOVW (or, when \p Top, MOVT) writes to ip.
std::optional<uint32_t> armMoveToIP(uint32_t Word, bool Top) {
  if ((Word & 0xFFF0F000u) != (Top ? 0xE340C000u : 0xE300C000u))
    return std::nullopt;
  return ((Word >> 4) & 0xF000u) | (Word & 0xFFFu);
}

/// The immediate a Thumb-2 MOVW (or, when \p Top, MOVT) at \p Insn writes to
/// ip.
std::optional<uint32_t> thumbMoveToIP(const uint8_t *Insn, bool Top) {
  const uint16_t First = readLE<uint16_t>(Insn);
  const uint16_t Second = readLE<uint16_t>(Insn + 2);
  if ((First & 0xFBF0) != (Top ? 0xF2C0 : 0xF240) ||
      (Second & 0x8F00) != 0x0C00)
    return std::nullopt;
  return (uint32_t(First & 0xF) << 12) | (uint32_t(First & 0x400) << 1) |
         (uint32_t(Second & 0x7000) >> 4) | (Second & 0xFFu);
}

/// The routine an ARM-state linker thunk at \p Address forwards to: an
/// unconditional B, `ldr pc, [pc, #-4]` and the address after it (ARMv5 and
/// GNU long branches), or lld's `movw ip; movt ip; [add ip, ip, pc;] bx ip`.
std::optional<Branch> armThunkTarget(const BinaryImage &Img, uint64_t Address) {
  if (std::optional<Branch> Jump = armBranch(Img, Address, /*JumpOnly=*/true))
    return Jump;
  auto Word = [&](unsigned Index) -> std::optional<uint32_t> {
    const uint8_t *Insn = Img.readVA(Address + 4 * Index, 4);
    if (!Insn)
      return std::nullopt;
    return readLE<uint32_t>(Insn);
  };
  const std::optional<uint32_t> First = Word(0), Second = Word(1);
  if (!First || !Second)
    return std::nullopt;
  if (*First == 0xE51FF004u)
    return interworkingTarget(Img, *Second);
  constexpr uint32_t BxIP = 0xE12FFF1Cu;
  const std::optional<uint32_t> Low = armMoveToIP(*First, /*Top=*/false);
  const std::optional<uint32_t> High =
      Low ? armMoveToIP(*Second, /*Top=*/true) : std::nullopt;
  const std::optional<uint32_t> Third = High ? Word(2) : std::nullopt;
  if (!Third)
    return std::nullopt;
  const uint64_t Value = (uint64_t(*High) << 16) | *Low;
  if (*Third == BxIP)
    return interworkingTarget(Img, Value);
  // `add ip, ip, pc` reads pc eight bytes past itself.
  if (*Third == 0xE08CC00Fu && Word(3) == BxIP)
    return interworkingTarget(Img, Value + Address + 16);
  return std::nullopt;
}

/// The routine a Thumb linker thunk at \p Address forwards to: a B.W, or
/// lld's `movw ip; movt ip; [add ip, pc;] bx ip`.
std::optional<Branch> thumbThunkTarget(const BinaryImage &Img,
                                       uint64_t Address) {
  if (std::optional<Branch> Jump = thumbBranch(Img, Address, /*JumpOnly=*/true))
    return Jump;
  const uint8_t *Moves = Img.readVA(Address, 8);
  if (!Moves)
    return std::nullopt;
  const std::optional<uint32_t> Low = thumbMoveToIP(Moves, /*Top=*/false);
  const std::optional<uint32_t> High =
      Low ? thumbMoveToIP(Moves + 4, /*Top=*/true) : std::nullopt;
  if (!High)
    return std::nullopt;
  auto Halfword = [&](unsigned Index) -> std::optional<uint16_t> {
    const uint8_t *Insn = Img.readVA(Address + 8 + 2 * Index, 2);
    if (!Insn)
      return std::nullopt;
    return readLE<uint16_t>(Insn);
  };
  constexpr uint16_t BxIP = 0x4760;
  const uint64_t Value = (uint64_t(*High) << 16) | *Low;
  const std::optional<uint16_t> Third = Halfword(0);
  if (Third == BxIP)
    return interworkingTarget(Img, Value);
  // `add ip, pc` reads pc four bytes past itself.
  if (Third == 0x44FC && Halfword(1) == BxIP)
    return interworkingTarget(Img, Value + Address + 12);
  return std::nullopt;
}

/// The routine an AArch64 linker thunk at \p Address forwards to: a B,
/// `adrp x16; add x16, x16, #lo12; br x16`, or `ldr x16, #8; br x16` and the
/// address after them.
std::optional<Branch> aarch64ThunkTarget(const BinaryImage &Img,
                                         uint64_t Address) {
  const uint8_t *Insn = Img.readVA(Address, 4);
  if (!Insn)
    return std::nullopt;
  const uint32_t First = readLE<uint32_t>(Insn);
  if ((First & 0xFC000000u) == 0x14000000u)
    return Branch{Address + static_cast<uint64_t>(
                                signExtend(First & 0x03FFFFFFu, 26) * 4)};
  const uint8_t *Rest = Img.readVA(Address, 12);
  if (!Rest)
    return std::nullopt;
  const uint32_t Second = readLE<uint32_t>(Rest + 4);
  const uint32_t Third = readLE<uint32_t>(Rest + 8);
  constexpr uint32_t BrX16 = 0xD61F0200u;
  if ((First & 0x9F00001Fu) == 0x90000010u &&
      (Second & 0xFFC003FFu) == 0x91000210u && Third == BrX16) {
    const uint64_t Immediate =
        (uint64_t((First >> 5) & 0x7FFFFu) << 2) | ((First >> 29) & 3u);
    const uint64_t Page =
        (Address & ~uint64_t(0xFFF)) +
        static_cast<uint64_t>(signExtend(Immediate, 21) * 4096);
    return Branch{Page + ((Second >> 10) & 0xFFFu)};
  }
  if (First == 0x58000050u && Second == BrX16) {
    const uint8_t *Literal = Img.readVA(Address + 8, 8);
    if (Literal)
      return Branch{readLE<uint64_t>(Literal)};
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
  case Arch::X64: {
    const uint8_t *Insn = Img.readVA(To.Target, 5);
    if (!Insn || Insn[0] != 0xE9)
      return std::nullopt;
    const int64_t Disp = readLE<int32_t>(Insn + 1);
    return Branch{
        wrapToImage(Img, To.Target + 5 + static_cast<uint64_t>(Disp))};
  }
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

void SignatureDB::checkReferences(const BinaryImage &Img) {
  bool AnyReferences = false;
  for (size_t Module : MatchModules)
    AnyReferences |= Module != NoModule && Modules[Module].ReferenceCount != 0;
  if (!AnyReferences)
    return;

  // What the bytes alone settle, which is what a reference is checked
  // against: an address two matches name differently names nothing.
  const std::unordered_map<uint64_t, SettledRoutine> Settled = settleRoutines();

  // The modules that describe each routine the matches call from its start,
  // to confirm a reference by the named routine's own pattern.  Only a
  // called name is ever looked up.
  std::unordered_map<std::string_view, std::vector<size_t>> ByName;
  for (size_t I = 0; I < Matches.size(); ++I)
    if (MatchModules[I] != NoModule)
      for (const StoredName &Ref : Modules[MatchModules[I]].references())
        ByName.try_emplace(Ref.Name);
  for (size_t I = 0; I < Modules.size(); ++I)
    for (const StoredName &Name : Modules[I].publicNames())
      if (Name.Offset == 0)
        if (const auto It = ByName.find(Name.Name); It != ByName.end())
          It->second.push_back(I);

  auto PatternAt = [&](std::string_view Name, uint64_t Address) {
    const auto Candidates = ByName.find(Name);
    const Segment *Seg = Img.getSegmentFor(Address);
    if (Candidates == ByName.end() || !Seg || !Seg->isExecutable() ||
        Address < Seg->VA || Address - Seg->VA >= Seg->Data.size())
      return false;
    const size_t Offset = static_cast<size_t>(Address - Seg->VA);
    for (size_t Module : Candidates->second)
      if (SignatureMatcher::matchPattern(Modules[Module],
                                         Seg->Data.data() + Offset,
                                         Seg->Data.size() - Offset))
        return true;
    return false;
  };

  // A Thumb routine may be entered with the interworking bit set.
  auto SettledAt = [&](uint64_t Target) {
    auto It = Settled.find(Target);
    if (It == Settled.end() && Img.Arch == Arch::ARM)
      It = Settled.find(Target | 1);
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
      return Imp->Name == Name.substr(0, Name.find('@'))
                 ? ReferenceVerdict::Confirmed
                 : ReferenceVerdict::Contradicted;
    }
    // A library calls a routine by whichever of its names it uses, so any
    // name the routine settled with confirms the call.
    if (const auto It = SettledAt(Target); It != Settled.end())
      return It->second.Names.count(Name) ? ReferenceVerdict::Confirmed
                                          : ReferenceVerdict::Contradicted;
    // A routine the image replaced (operator new, say) does not match the
    // library's pattern and is still the routine called, so a pattern that
    // does not match contradicts nothing.
    return PatternAt(Name, Target) ? ReferenceVerdict::Confirmed
                                   : ReferenceVerdict::Unknown;
  };

  std::vector<SigMatch> Kept;
  std::vector<size_t> KeptModules;
  Kept.reserve(Matches.size());
  for (size_t I = 0; I < Matches.size(); ++I) {
    const size_t Module = MatchModules[I];
    const llvm::ArrayRef<StoredName> References =
        Module == NoModule ? llvm::ArrayRef<StoredName>()
                           : Modules[Module].references();
    bool Contradicted = false;
    size_t Confirmed = 0;
    if (!References.empty()) {
      // Every public name of a module shares its references; the module's
      // start is the match address less the name's offset.
      uint64_t Start = Matches[I].Address;
      for (const StoredName &Name : Modules[Module].publicNames())
        if (Name.Name == Matches[I].Name) {
          Start = Matches[I].Address - Name.Offset;
          break;
        }
      if (Img.Arch == Arch::ARM)
        Start &= ~uint64_t(1);
      for (const StoredName &Ref : References) {
        const std::optional<Branch> To = branchTarget(Img, Start, Ref.Offset);
        if (!To)
          continue;
        ReferenceVerdict Verdict = Judge(Ref.Name, To->Target);
        if (Verdict == ReferenceVerdict::Unknown)
          if (const std::optional<Branch> Next = thunkTarget(Img, *To))
            Verdict = Judge(Ref.Name, Next->Target);
        if (Verdict == ReferenceVerdict::Contradicted) {
          Contradicted = true;
          break;
        }
        Confirmed += Verdict == ReferenceVerdict::Confirmed;
      }
    }
    if (Contradicted)
      continue;
    Kept.push_back(std::move(Matches[I]));
    Kept.back().Confirmed =
        !References.empty() && Confirmed == References.size();
    KeptModules.push_back(Module);
  }
  Matches = std::move(Kept);
  MatchModules = std::move(KeptModules);
}
