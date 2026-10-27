//===- MapFile.cpp - A linker MAP and the image it describes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reads a MAP in the dialect it is written in, against the layout of the
/// image it is loaded for, and holds what it states against that image.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/MapFile.h"

#include "MapParsers.h"

#include "neverd/debug/DebugFileIdentity.h"
#include "neverd/debug/MapDebugContextBase.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/MemoryBuffer.h"

namespace neverd {

namespace {

/// The names a MAP gives, as a debug context.
class MapFileContext final : public MapDebugContextBase {
public:
  std::map<va_t, FunctionSym> &functions() { return Functions; }
  std::map<va_t, DataObjectSym> &dataObjects() { return DataObjects; }
  std::map<va_t, SourceLoc> &sourceLocations() { return SourceLocations; }
  /// Settle the context once its names are in.
  void finish() {
    inferFunctionSizes();
    Loaded = !Functions.empty();
  }
};

void addCOFFRegions(const llvm::object::COFFObjectFile &Obj,
                    MapImageLayout &Layout) {
  Layout.Format = BinaryFormat::COFF;
  Layout.Base = Obj.getImageBase();
  if (Obj.getPE32Header() || Obj.getPE32PlusHeader())
    Layout.Stamp = Obj.getTimeDateStamp();
  for (const llvm::object::SectionRef &Ref : Obj.sections()) {
    const llvm::object::coff_section *Sec = Obj.getCOFFSection(Ref);
    MapImageLayout::Region Region;
    if (auto Name = Obj.getSectionName(Sec))
      Region.Name = Name->str();
    else
      llvm::consumeError(Name.takeError());
    Region.Address = Layout.Base + Sec->VirtualAddress;
    Region.Size = Sec->VirtualSize ? Sec->VirtualSize : Sec->SizeOfRawData;
    Region.FileSize = Sec->SizeOfRawData;
    Region.Executable =
        Sec->Characteristics &
        (llvm::COFF::IMAGE_SCN_MEM_EXECUTE | llvm::COFF::IMAGE_SCN_CNT_CODE);
    Layout.Sections.push_back(std::move(Region));
  }
}

template <typename ELFT>
void addELFRegions(const llvm::object::ELFObjectFile<ELFT> &Obj,
                   MapImageLayout &Layout) {
  Layout.Format = BinaryFormat::ELF;
  const auto &File = Obj.getELFFile();
  if (auto Sections = File.sections()) {
    for (const auto &Sec : *Sections) {
      if (Sec.sh_type == llvm::ELF::SHT_NULL ||
          !(Sec.sh_flags & llvm::ELF::SHF_ALLOC))
        continue;
      MapImageLayout::Region Region;
      if (auto Name = File.getSectionName(Sec))
        Region.Name = Name->str();
      else
        llvm::consumeError(Name.takeError());
      Region.Address = Sec.sh_addr;
      Region.Size = Sec.sh_size;
      Region.Executable = Sec.sh_flags & llvm::ELF::SHF_EXECINSTR;
      Layout.Sections.push_back(std::move(Region));
    }
  } else {
    llvm::consumeError(Sections.takeError());
  }
  if (!Layout.Sections.empty())
    return;
  // An image stripped of its section headers keeps its loadable segments.
  if (auto Headers = File.program_headers()) {
    for (const auto &Header : *Headers) {
      if (Header.p_type != llvm::ELF::PT_LOAD)
        continue;
      MapImageLayout::Region Region;
      Region.Name =
          kLoadSegmentPrefix.str() + std::to_string(Layout.Sections.size());
      Region.Address = Header.p_vaddr;
      Region.Size = Header.p_memsz;
      Region.Executable = Header.p_flags & llvm::ELF::PF_X;
      Layout.Sections.push_back(std::move(Region));
    }
  } else {
    llvm::consumeError(Headers.takeError());
  }
}

void addMachORegions(const llvm::object::MachOObjectFile &Obj,
                     MapImageLayout &Layout) {
  Layout.Format = BinaryFormat::MachO;
  for (const llvm::object::SectionRef &Ref : Obj.sections()) {
    const llvm::object::DataRefImpl Raw = Ref.getRawDataRefImpl();
    MapImageLayout::Region Region;
    Region.Segment = Obj.getSectionFinalSegmentName(Raw).str();
    if (auto Name = Obj.getSectionName(Raw))
      Region.Name = Name->str();
    else
      llvm::consumeError(Name.takeError());
    Region.Address = Ref.getAddress();
    Region.Size = Ref.getSize();
    const uint32_t Flags =
        Obj.is64Bit() ? Obj.getSection64(Raw).flags : Obj.getSection(Raw).flags;
    Region.Executable = Flags & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                                 llvm::MachO::S_ATTR_SOME_INSTRUCTIONS);
    Layout.Sections.push_back(std::move(Region));
  }
}

/// Fill \p Layout from \p Obj; false for an object of another format.
bool addRegions(const llvm::object::Binary &Obj, MapImageLayout &Layout) {
  if (const auto *COFF = llvm::dyn_cast<llvm::object::COFFObjectFile>(&Obj)) {
    addCOFFRegions(*COFF, Layout);
    return true;
  }
  if (const auto *MachO = llvm::dyn_cast<llvm::object::MachOObjectFile>(&Obj)) {
    addMachORegions(*MachO, Layout);
    return true;
  }
  if (const auto *ELF = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(&Obj))
    addELFRegions(*ELF, Layout);
  else if (const auto *ELF =
               llvm::dyn_cast<llvm::object::ELF32BEObjectFile>(&Obj))
    addELFRegions(*ELF, Layout);
  else if (const auto *ELF =
               llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(&Obj))
    addELFRegions(*ELF, Layout);
  else if (const auto *ELF =
               llvm::dyn_cast<llvm::object::ELF64BEObjectFile>(&Obj))
    addELFRegions(*ELF, Layout);
  else
    return false;
  return true;
}

/// link.exe and lld-link state the image's build stamp and base in their
/// header: "Timestamp is 5f5a6b7c (...)", "Preferred load address is ...".
void readSegmentedHeader(llvm::StringRef Content, MapFacts &Facts) {
  constexpr llvm::StringLiteral StampLine("Timestamp is ");
  constexpr llvm::StringLiteral BaseLine("Preferred load address is ");
  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;
    llvm::StringRef Trimmed = Line.trim();
    // The header ends where the names begin.
    if (MapDebugContextBase::isCOFFMapHeader(Trimmed))
      break;
    if (Trimmed.consume_front(StampLine)) {
      uint32_t Stamp;
      if (!Trimmed.take_while(llvm::isHexDigit).getAsInteger(16, Stamp))
        Facts.Stamp = Stamp;
    } else if (Trimmed.consume_front(BaseLine)) {
      uint64_t Base;
      if (!Trimmed.take_while(llvm::isHexDigit).getAsInteger(16, Base))
        Facts.PreferredBase = Base;
    }
  }
}

/// The one region of \p Layout named as \p Placed is, if exactly one is.
const MapImageLayout::Region *uniqueRegion(const MapImageLayout &Layout,
                                           const MapSection &Placed) {
  const MapImageLayout::Region *Found = nullptr;
  for (const MapImageLayout::Region &Region : Layout.Sections) {
    if (Region.Name != Placed.Name ||
        (!Placed.Segment.empty() && Region.Segment != Placed.Segment))
      continue;
    if (Found)
      return nullptr;
    Found = &Region;
  }
  return Found;
}

/// Hold \p Facts and the names of \p Names against \p Layout.
MapCheck checkMap(const MapFacts &Facts, const MapFileContext &Names,
                  const MapImageLayout &Layout) {
  MapCheck Check;
  const std::vector<FunctionSym> Functions = Names.allFunctions();
  Check.Functions = Functions.size();
  Check.DataNames = Names.allDataObjects().size();
  const auto mismatch = [&](MapMismatch Reason) {
    Check.Match = MapMatch::Mismatch;
    Check.Reason = Reason;
    return Check;
  };

  // A recorded build stamp decides alone.
  if (Facts.Stamp && Layout.Stamp) {
    if (*Facts.Stamp != *Layout.Stamp) {
      Check.MapStamp = *Facts.Stamp;
      Check.ImageStamp = *Layout.Stamp;
      return mismatch(MapMismatch::Stamp);
    }
    Check.Match = MapMatch::Verified;
  }
  if (Layout.Sections.empty())
    return Check;

  // The sections the MAP places, against the image's of the same name.
  size_t Compared = 0;
  for (const MapSection &Placed : Facts.Sections) {
    const MapImageLayout::Region *Region = uniqueRegion(Layout, Placed);
    if (!Region || (Placed.Address == 0 && Placed.Size == 0))
      continue;
    const uint64_t Address =
        Facts.RelativeSections ? Layout.Base + Placed.Address : Placed.Address;
    if (Address != Region->Address || !Region->sizedAs(Placed.Size)) {
      Check.MapPlaced = Placed;
      Check.MapPlaced.Address = Address;
      Check.ImagePlaced = {Region->Segment, Region->Name, Region->Address,
                           Region->Size};
      return mismatch(MapMismatch::Section);
    }
    ++Compared;
  }

  // Names that land outside the image's code say the MAP describes another
  // layout, however few sections it states.
  for (const FunctionSym &Function : Functions)
    if (!Layout.isCode(Function.Addr))
      ++Check.Outside;
  if (Check.Functions != 0 && Check.Outside * 2 > Check.Functions)
    return mismatch(MapMismatch::Outside);
  if (Compared != 0 && Check.Match == MapMatch::Unverified)
    Check.Match = MapMatch::Consistent;
  return Check;
}

} // anonymous namespace

const char *mapDialectName(MapDialect Dialect) {
  switch (Dialect) {
#define NEVERD_MAP_DIALECT(Name, Spelling, Linker)                             \
  case MapDialect::Name:                                                       \
    return Spelling;
#include "neverd/debug/MapDialects.def"
  }
  return "msvc";
}

const char *mapMatchName(MapMatch Match) {
  switch (Match) {
#define NEVERD_MAP_MATCH(Name, Spelling)                                       \
  case MapMatch::Name:                                                         \
    return Spelling;
#include "neverd/debug/MapMatches.def"
  }
  return "unverified";
}

const char *mapMismatchName(MapMismatch Reason) {
  switch (Reason) {
#define NEVERD_MAP_MISMATCH(Name, Spelling)                                    \
  case MapMismatch::Name:                                                      \
    return Spelling;
#include "neverd/debug/MapMatches.def"
  }
  return "";
}

MapDialect detectMapDialect(llvm::StringRef Content) {
  if (Content.ltrim().starts_with("# Path:"))
    return MapDialect::LD64;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;
    if (isGNUldMapHeader(Line))
      return MapDialect::GNU;
    if (isELFMapHeader(Line))
      return MapDialect::LLD;
    if (isLLDMapHeader(Line))
      return MapDialect::LLDCOFF;
    if (MapDebugContextBase::isCOFFMapHeader(Line))
      return MapDialect::MSVC;
  }

  if (Content.contains("VMA") && Content.contains("LMA"))
    return MapDialect::LLD;
  return MapDialect::MSVC;
}

bool MapImageLayout::isCode(va_t Addr) const {
  return llvm::any_of(Sections, [&](const Region &R) {
    return R.Executable && R.contains(Addr);
  });
}

bool MapImageLayout::isMapped(va_t Addr) const {
  return llvm::any_of(Sections,
                      [&](const Region &R) { return R.contains(Addr); });
}

llvm::Expected<MapImageLayout> readMapImageLayout(llvm::MemoryBufferRef Bytes,
                                                  std::optional<Arch> Slice) {
  const auto failure = [](const char *Message) {
    return llvm::make_error<llvm::StringError>(Message,
                                               llvm::inconvertibleErrorCode());
  };
  MapImageLayout Layout;
  auto Universal = llvm::object::MachOUniversalBinary::create(Bytes);
  if (!Universal) {
    llvm::consumeError(Universal.takeError());
    auto Object = llvm::object::createBinary(Bytes);
    if (!Object)
      return Object.takeError();
    if (!addRegions(**Object, Layout))
      return failure("not a PE, ELF or Mach-O image");
    return Layout;
  }
  // The slice of the architecture asked for, else the first that reads.
  std::unique_ptr<llvm::object::MachOObjectFile> Chosen;
  for (const auto &Member : (*Universal)->objects()) {
    auto Object = Member.getAsObjectFile();
    if (!Object) {
      llvm::consumeError(Object.takeError());
      continue;
    }
    const bool Wanted = Slice && isTripleArchOf((*Object)->getArch(), *Slice);
    if (Wanted || !Chosen)
      Chosen = std::move(*Object);
    if (Wanted)
      break;
  }
  if (!Chosen)
    return failure("the universal binary has no readable slice");
  addMachORegions(*Chosen, Layout);
  return Layout;
}

MapImageLayout mapImageLayout(const BinaryImage &Img,
                              const std::filesystem::path &File) {
  if (Img.Format == BinaryFormat::COFF || Img.Format == BinaryFormat::ELF ||
      Img.Format == BinaryFormat::MachO) {
    std::unique_ptr<llvm::MemoryBuffer> Read;
    llvm::MemoryBufferRef Bytes;
    if (!Img.Raw.empty()) {
      Bytes = llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Img.Raw.data()),
                          Img.Raw.size()),
          "image");
    } else if (auto Buffer = llvm::MemoryBuffer::getFile(
                   pathToUTF8(File), /*IsText=*/false,
                   /*RequiresNullTerminator=*/false)) {
      Read = std::move(*Buffer);
      Bytes = Read->getMemBufferRef();
    }
    if (Bytes.getBufferSize() != 0) {
      if (auto Layout = readMapImageLayout(Bytes, Img.Arch))
        return std::move(*Layout);
      else
        llvm::consumeError(Layout.takeError());
    }
  }
  // A binary file has the segments its loader placed, and nothing more.
  MapImageLayout Layout;
  Layout.Format = Img.Format;
  Layout.Base = Img.Base;
  for (const Segment &Seg : Img.Segments) {
    MapImageLayout::Region Region;
    Region.Name = Seg.Name;
    Region.Address = Seg.VA;
    Region.Size = Seg.Size;
    Region.Executable = hasFlag(Seg.Flags, SegmentFlags::Executable);
    Layout.Sections.push_back(std::move(Region));
  }
  return Layout;
}

MapRead readMapFile(const std::filesystem::path &Path,
                    const MapImageLayout &Layout) {
  MapRead Result;
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/true);
  if (!Buffer) {
    Result.Error =
        "cannot read " + pathToUTF8(Path) + ": " + Buffer.getError().message();
    return Result;
  }
  const llvm::StringRef Content = (*Buffer)->getBuffer();
  auto Names = std::make_unique<MapFileContext>();
  MapFacts &Facts = Result.Facts;
  Facts.Dialect = detectMapDialect(Content);
  // lld-link states image-relative addresses; the image places them.
  const uint64_t RelativeBase =
      Layout.Format == BinaryFormat::COFF ? Layout.Base : 0;
  switch (Facts.Dialect) {
  case MapDialect::MSVC:
    readSegmentedHeader(Content, Facts);
    MapDebugContextBase::parseCOFFMapContent(Content, Names->functions(),
                                             Layout.Base, &Names->dataObjects(),
                                             &Layout);
    MapDebugContextBase::parseCOFFMapLineNumbers(
        Content, Names->sourceLocations(), Layout.Base, &Layout);
    break;
  case MapDialect::LLDCOFF:
    Facts.RelativeSections = true;
    parseCOFFLLDMap(Content, Names->functions(), RelativeBase, &Facts.Sections);
    break;
  case MapDialect::LLD:
    parseELFMap(Content, Names->functions(), &Facts.Sections);
    break;
  case MapDialect::GNU:
    parseGNUldMap(Content, Names->functions(), &Facts.Sections);
    break;
  case MapDialect::LD64:
    parseMachOMap(Content, Names->functions(), &Facts.Sections);
    break;
  }
  Names->finish();
  Result.Check = checkMap(Facts, *Names, Layout);
  if (Names->hasInfo())
    Result.Context = std::move(Names);
  return Result;
}

} // namespace neverd
