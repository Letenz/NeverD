//===- PDB20Loader.cpp - VC6 JG / PDB 2.00 names-only loader -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/debug/PDBLoader.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/PDBTypes.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace neverd {
namespace {

llvm::Error pdb20Error(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "pdb20: " + Message);
}

llvm::Expected<std::vector<uint8_t>>
readIndexedStream(llvm::pdb::PDBFile &File, uint32_t Index) {
  if (Index >= File.getNumStreams())
    return pdb20Error("stream index out of range");
  const uint32_t Size = File.getStreamByteSize(Index);
  if (Size == 0 || Size == UINT32_MAX)
    return std::vector<uint8_t>();
  auto StreamOr = File.safelyCreateIndexedStream(Index);
  if (!StreamOr)
    return StreamOr.takeError();
  llvm::BinaryStreamReader Reader(**StreamOr);
  llvm::ArrayRef<uint8_t> Bytes;
  if (auto EC = Reader.readBytes(Bytes, Size))
    return std::move(EC);
  return std::vector<uint8_t>(Bytes.begin(), Bytes.end());
}

uint32_t read32(const std::vector<uint8_t> &Bytes, uint32_t Off) {
  return llvm::support::endian::read32le(Bytes.data() + Off);
}

struct SectionHdr {
  std::string Name;
  uint32_t VirtualAddress = 0;
  uint32_t VirtualSize = 0;
  uint32_t SizeOfRawData = 0;
  uint32_t PointerToRawData = 0;
  uint32_t Characteristics = 0;
};

llvm::Expected<std::vector<SectionHdr>>
parseSectionHeaders(const std::vector<uint8_t> &Bytes) {
  if (Bytes.size() % 40 != 0)
    return pdb20Error("section header stream is not a multiple of 40 bytes");
  std::vector<SectionHdr> Sections;
  Sections.reserve(Bytes.size() / 40);
  for (size_t I = 0; I < Bytes.size(); I += 40) {
    SectionHdr S;
    char Name[9] = {};
    std::memcpy(Name, Bytes.data() + I, 8);
    S.Name = Name;
    S.VirtualSize = read32(Bytes, static_cast<uint32_t>(I + 8));
    S.VirtualAddress = read32(Bytes, static_cast<uint32_t>(I + 12));
    S.SizeOfRawData = read32(Bytes, static_cast<uint32_t>(I + 16));
    S.PointerToRawData = read32(Bytes, static_cast<uint32_t>(I + 20));
    S.Characteristics = read32(Bytes, static_cast<uint32_t>(I + 36));
    Sections.push_back(std::move(S));
  }
  return Sections;
}

llvm::Error validateSections(const BinaryImage &Image,
                             llvm::ArrayRef<SectionHdr> PDBSections) {
  if (PDBSections.size() != Image.Sections.size())
    return pdb20Error("section table does not match loaded PE image");
  for (size_t I = 0; I < Image.Sections.size(); ++I) {
    const Section &Loaded = Image.Sections[I];
    const SectionHdr &Recorded = PDBSections[I];
    constexpr uint64_t MaxCOFFField = std::numeric_limits<uint32_t>::max();
    if (Loaded.VA < Image.Base || Loaded.Size > MaxCOFFField ||
        Loaded.FileOff > MaxCOFFField || Loaded.FileSz > MaxCOFFField ||
        Recorded.Name != Loaded.Name ||
        static_cast<uint64_t>(Recorded.VirtualAddress) !=
            Loaded.VA - Image.Base ||
        static_cast<uint64_t>(Recorded.VirtualSize) != Loaded.Size ||
        static_cast<uint64_t>(Recorded.PointerToRawData) != Loaded.FileOff ||
        static_cast<uint64_t>(Recorded.SizeOfRawData) != Loaded.FileSz ||
        Recorded.Characteristics != Loaded.Type)
      return pdb20Error("section table does not match loaded PE image");
  }
  return llvm::Error::success();
}

struct ModuleRec {
  int16_t Stream = -1;
  uint32_t SymBytes = 0;
};

llvm::Expected<std::vector<ModuleRec>>
parseModules(llvm::ArrayRef<uint8_t> Modi) {
  std::vector<ModuleRec> Mods;
  uint32_t Off = 0;
  while (Off + 64 <= Modi.size()) {
    ModuleRec M;
    M.Stream = static_cast<int16_t>(
        llvm::support::endian::read16le(Modi.data() + Off + 34));
    M.SymBytes =
        llvm::support::endian::read32le(Modi.data() + Off + 36);
    uint32_t P = Off + 64;
    while (P < Modi.size() && Modi[P] != 0)
      ++P;
    if (P >= Modi.size())
      return pdb20Error("truncated module name");
    ++P;
    while (P < Modi.size() && Modi[P] != 0)
      ++P;
    if (P >= Modi.size())
      return pdb20Error("truncated module object name");
    uint32_t End = (P + 1 + 3) & ~3u;
    if (End <= Off)
      break;
    Mods.push_back(M);
    Off = End;
  }
  return Mods;
}

struct ProcRec {
  uint16_t Segment = 0;
  uint32_t Offset = 0;
  uint32_t Size = 0;
  std::string Name;
};

bool parseLengthPrefixed(llvm::ArrayRef<uint8_t> Rec, uint32_t NameOff,
                         std::string &Name) {
  if (NameOff >= Rec.size())
    return false;
  const uint8_t Len = Rec[NameOff];
  if (NameOff + 1u + Len > Rec.size())
    return false;
  Name.assign(reinterpret_cast<const char *>(Rec.data() + NameOff + 1), Len);
  return !Name.empty();
}

void walkSymbols(llvm::ArrayRef<uint8_t> Bytes, uint32_t Start, uint32_t Limit,
                 std::vector<ProcRec> &Out, bool PublicsAsFunctions) {
  uint32_t Off = Start;
  const uint32_t End = std::min<uint32_t>(Limit, Bytes.size());
  while (Off + 4 <= End) {
    const uint16_t Len =
        llvm::support::endian::read16le(Bytes.data() + Off);
    const uint16_t Kind =
        llvm::support::endian::read16le(Bytes.data() + Off + 2);
    if (Len < 2 || Off + 2u + Len > Bytes.size())
      break;
    llvm::ArrayRef<uint8_t> Rec(Bytes.data() + Off + 4, Len - 2);
    using SK = llvm::codeview::SymbolKind;
    if ((Kind == static_cast<uint16_t>(SK::S_GPROC32_ST) ||
         Kind == static_cast<uint16_t>(SK::S_LPROC32_ST)) &&
        Rec.size() >= 36) {
      ProcRec P;
      P.Size = llvm::support::endian::read32le(Rec.data() + 12);
      P.Offset = llvm::support::endian::read32le(Rec.data() + 28);
      P.Segment = llvm::support::endian::read16le(Rec.data() + 32);
      if (parseLengthPrefixed(Rec, 35, P.Name))
        Out.push_back(std::move(P));
    } else if (PublicsAsFunctions &&
               Kind == static_cast<uint16_t>(SK::S_PUB32_ST) &&
               Rec.size() >= 11) {
      const uint32_t Flags = llvm::support::endian::read32le(Rec.data());
      const bool IsFunc =
          (Flags & static_cast<uint32_t>(
                       llvm::codeview::PublicSymFlags::Function)) != 0;
      if (IsFunc) {
        ProcRec P;
        P.Offset = llvm::support::endian::read32le(Rec.data() + 4);
        P.Segment = llvm::support::endian::read16le(Rec.data() + 8);
        if (parseLengthPrefixed(Rec, 10, P.Name))
          Out.push_back(std::move(P));
      }
    }
    Off += 2u + Len;
  }
}

bool machineAcceptable(uint16_t Machine, Arch ImageArch) {
  if (Machine == 0)
    return ImageArch == Arch::X86;
  using llvm::pdb::PDB_Machine;
  switch (ImageArch) {
  case Arch::X64:
    return Machine == static_cast<uint16_t>(PDB_Machine::Amd64);
  case Arch::X86:
    return Machine == static_cast<uint16_t>(PDB_Machine::x86);
  case Arch::AArch64:
    return Machine == static_cast<uint16_t>(PDB_Machine::Arm64);
  case Arch::ARM:
    return Machine == static_cast<uint16_t>(PDB_Machine::ArmNT);
  default:
    return false;
  }
}

} // namespace

llvm::Expected<std::unique_ptr<PDBDebugContext>>
loadPdb20DebugContext(const std::filesystem::path &PdbPath,
                      const BinaryImage &Image) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable)
    return pdb20Error("strict PDB loading requires a linked PE image");
  if (Image.DynInfo.CodeViewPDBIdentityState != PDBIdentityState::Unique ||
      !Image.DynInfo.CodeViewPDBIdentity ||
      Image.DynInfo.CodeViewPDBIdentity->Kind != PDBIdentityKind::NB10)
    return pdb20Error(Image.DynInfo.CodeViewPDBIdentityState ==
                              PDBIdentityState::Ambiguous
                          ? "PE CodeView identity is malformed or ambiguous"
                          : "PE image has no unique CodeView NB10 identity");

  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  if (auto Err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           llvm::StringRef(PdbPath.string()),
                                           Session))
    return llvm::createFileError(PdbPath.string(), std::move(Err));

  auto *Native = static_cast<llvm::pdb::NativeSession *>(Session.get());
  llvm::pdb::PDBFile &File = Native->getPDBFile();
  if (!File.isPdb20())
    return pdb20Error("container is not PDB 2.00 JG");

  auto InfoOr = File.getPDBInfoStream();
  if (!InfoOr)
    return pdb20Error("cannot read PDB Info stream: " +
                      llvm::toString(InfoOr.takeError()));
  auto &Info = *InfoOr;
  PDBBuildIdentity Actual;
  Actual.Kind = PDBIdentityKind::NB10;
  Actual.Signature = Info.getSignature();
  Actual.Age = Info.getAge();
  if (!Actual.isValid())
    return pdb20Error("PDB Info stream has an invalid signature/age");
  if (Actual != *Image.DynInfo.CodeViewPDBIdentity)
    return pdb20Error(
        "PDB Info signature/age does not match PE CodeView NB10");

  auto DbiOr = readIndexedStream(File, llvm::pdb::StreamDBI);
  if (!DbiOr)
    return DbiOr.takeError();
  const std::vector<uint8_t> &DBI = *DbiOr;
  if (DBI.size() < sizeof(llvm::pdb::DbiStreamHeader))
    return pdb20Error("DBI stream is truncated");

  llvm::pdb::DbiStreamHeader Header{};
  std::memcpy(&Header, DBI.data(), sizeof(Header));
  if (static_cast<int32_t>(Header.VersionSignature) != -1)
    return pdb20Error("DBI signature is not 0xFFFFFFFF");
  if (Header.Age != Info.getAge())
    return pdb20Error("DBI age does not match PDB Info age");
  if (!machineAcceptable(Header.MachineType, Image.Arch))
    return pdb20Error("DBI machine does not match loaded PE image");

  const int32_t ModiSize = Header.ModiSubstreamSize;
  const int32_t ScSize = Header.SecContrSubstreamSize;
  const int32_t SecMapSize = Header.SectionMapSize;
  const int32_t FileInfoSize = Header.FileInfoSize;
  const int32_t TypeServerSize = Header.TypeServerSize;
  const int32_t ECSize = Header.ECSubstreamSize;
  const int32_t DbgSize = Header.OptionalDbgHdrSize;
  if (ModiSize < 0 || ScSize < 0 || SecMapSize < 0 || FileInfoSize < 0 ||
      TypeServerSize < 0 || ECSize < 0 || DbgSize < 0)
    return pdb20Error("DBI substream size is negative");
  const uint64_t AfterHeader = 64ull + static_cast<uint32_t>(ModiSize) +
                               static_cast<uint32_t>(ScSize) +
                               static_cast<uint32_t>(SecMapSize) +
                               static_cast<uint32_t>(FileInfoSize) +
                               static_cast<uint32_t>(TypeServerSize) +
                               static_cast<uint32_t>(ECSize);
  if (AfterHeader + static_cast<uint32_t>(DbgSize) > DBI.size())
    return pdb20Error("DBI substreams overrun the stream");

  uint32_t SectionStream = llvm::pdb::kInvalidStreamIndex;
  if (DbgSize >= 12) {
    SectionStream = llvm::support::endian::read16le(
        DBI.data() + static_cast<size_t>(AfterHeader) + 10);
  }
  if (SectionStream == llvm::pdb::kInvalidStreamIndex)
    return pdb20Error("DBI has no section header debug stream");
  auto SecOr = readIndexedStream(File, SectionStream);
  if (!SecOr)
    return SecOr.takeError();
  auto SectionsOr = parseSectionHeaders(*SecOr);
  if (!SectionsOr)
    return SectionsOr.takeError();
  if (auto EC = validateSections(Image, *SectionsOr))
    return EC;

  auto ModsOr = parseModules(llvm::ArrayRef<uint8_t>(
      DBI.data() + 64, static_cast<size_t>(ModiSize)));
  if (!ModsOr)
    return ModsOr.takeError();

  std::vector<ProcRec> Procs;
  for (const ModuleRec &Mod : *ModsOr) {
    if (Mod.Stream <= 0 || Mod.SymBytes < 8)
      continue;
    auto ModStream = readIndexedStream(File, static_cast<uint32_t>(Mod.Stream));
    if (!ModStream)
      return ModStream.takeError();
    if (ModStream->size() < 4 || read32(*ModStream, 0) != 2)
      continue;
    walkSymbols(*ModStream, 4, Mod.SymBytes, Procs, false);
  }

  const uint16_t SymRecs = Header.SymRecordStreamIndex;
  if (SymRecs != 0 && SymRecs != llvm::pdb::kInvalidStreamIndex) {
    auto Gsym = readIndexedStream(File, SymRecs);
    if (!Gsym)
      return Gsym.takeError();
    walkSymbols(*Gsym, 0, static_cast<uint32_t>(Gsym->size()), Procs, true);
  }

  auto Ctx = std::unique_ptr<PDBDebugContext>(new PDBDebugContext());
  pdb_loader_detail::FunctionNameRegistry Names;
  std::set<va_t> Addresses;
  for (const ProcRec &P : Procs) {
    if (P.Segment == 0 || P.Segment > Image.Sections.size())
      continue;
    const Section &Owner = Image.Sections[P.Segment - 1];
    if (P.Offset >= Owner.Size)
      continue;
    const va_t VA = Owner.VA + P.Offset;
    Addresses.insert(VA);
    Names.observe(VA, P.Name);
  }
  std::vector<FunctionSym> Functions;
  for (const va_t VA : Addresses) {
    const std::optional<std::string> Name = Names.name(VA);
    if (!Name)
      continue;
    FunctionSym FS;
    FS.Addr = VA;
    FS.Name = *Name;
    Functions.push_back(std::move(FS));
  }
  Ctx->commitFunctions(std::move(Functions), true);
  return Ctx;
}

} // namespace neverd
