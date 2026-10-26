//===- NeverDCAPISections.cpp - Analysis image additions -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Transactional materialized sections in the active analysis image.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <stdexcept>

using namespace neverd;
using namespace neverd::sdk;

namespace {

void require(bool Condition, llvm::StringRef Message) {
  if (!Condition)
    throw std::invalid_argument(Message.str());
}

llvm::json::Value parseDocument(llvm::StringRef Text) {
  unsigned Depth = 0;
  bool Quoted = false, Escaped = false;
  for (const char C : Text) {
    if (Quoted) {
      if (Escaped)
        Escaped = false;
      else if (C == '\\')
        Escaped = true;
      else if (C == '"')
        Quoted = false;
    } else if (C == '"')
      Quoted = true;
    else if (C == '[' || C == '{')
      require(++Depth <= kMaxAnalysisSectionJsonDepth,
              "analysis section JSON nesting exceeds 64 levels");
    else if ((C == ']' || C == '}') && Depth)
      --Depth;
  }
  auto Parsed = llvm::json::parse(Text);
  if (!Parsed)
    throw std::invalid_argument(llvm::toString(Parsed.takeError()));
  return std::move(*Parsed);
}

uint64_t address(const llvm::json::Object &Object, llvm::StringRef Key) {
  const auto Text = Object.getString(Key);
  uint64_t Value = 0;
  require(Text && Text->starts_with("0x") && Text->size() <= 18 &&
              Text->size() > 2 && !Text->drop_front(2).getAsInteger(16, Value),
          "analysis section addresses must be hexadecimal strings");
  return Value;
}

std::string name(const llvm::json::Object &Object, llvm::StringRef Key) {
  const auto Text = Object.getString(Key);
  require(Text && !Text->empty() &&
              Text->size() <= kMaxAnalysisSectionNameBytes &&
              !Text->contains('\0'),
          "analysis section names must be nonempty bounded strings");
  return Text->str();
}

void fields(const llvm::json::Object &Object,
            std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &[Key, Value] : Object)
    require(std::find(Allowed.begin(), Allowed.end(), Key.str()) !=
                Allowed.end(),
            "unknown analysis section field");
}

bool overlaps(uint64_t Left, uint64_t LeftSize, uint64_t Right,
              uint64_t RightSize) {
  if (!LeftSize || !RightSize)
    return false;
  return Left <= Right ? Right - Left < LeftSize : Left - Right < RightSize;
}

/// All potentially allocating work precedes the swaps into the live image.
struct Update {
  std::vector<Segment> Segments;
  std::vector<Section> Sections;
  std::vector<Symbol> Symbols;
  llvm::json::Array Records;
  std::vector<FuncInfo> Functions;
  std::map<va_t, std::string> OriginalNames;

  explicit Update(const Session &S)
      : Segments(S.Img.Segments), Sections(S.Img.Sections),
        Symbols(S.Img.Symbols), Records(S.AnalysisSections) {}

  void prepareFunctions(const Session &S) {
    // Build the candidate list through the SDK authority, including analyst
    // create/delete decisions and the current export/name rules. Publication
    // remains allocation-free after the complete candidate has been prepared.
    Session Prepared;
    Prepared.Img = S.Img;
    Prepared.Img.Symbols = Symbols;
    Prepared.Loaded = true;
    Prepared.Renames = S.Renames;
    Prepared.FunctionEdits = S.FunctionEdits;
    Prepared.resetFunctionsFromImage();
    Functions = std::move(Prepared.Functions);
    OriginalNames = std::move(Prepared.OriginalNames);
  }

  void publish(Session &S) {
    prepareFunctions(S);
    S.clearPipeline();
    S.Img.Segments.swap(Segments);
    S.Img.Sections.swap(Sections);
    S.Img.Symbols.swap(Symbols);
    std::swap(S.AnalysisSections, Records);
    S.Functions.swap(Functions);
    S.OriginalNames.swap(OriginalNames);
    S.functionListChanged();
    S.DiscoveredFunctions.reset();
    S.ImageStrings.reset();
    S.LastPatch = {};
    S.RoundTrip = {};
  }
};

void requireImage(const Session *S) {
  require(S && S->Loaded, "no binary loaded");
  require(S->Img.Arch == Arch::X86 || S->Img.Arch == Arch::X64 ||
              S->Img.Arch == Arch::AArch64,
          "analysis sections require x86, x64 or AArch64 images");
}

std::string sourceHash(Session &S) {
  auto Digest = inputSha256(S);
  require(!Digest.empty(), "cannot read analysis section source identity");
  return Digest;
}

} // namespace

int neverd_analysis_section_add(neverd_session_t Sess, const char *Description,
                                const unsigned char *Data, size_t DataSize) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  try {
    requireImage(S);
    require(Description &&
                std::strlen(Description) <= kMaxAnalysisSectionDescriptionBytes,
            "invalid analysis section description");
    require(Data && DataSize && DataSize <= kMaxAnalysisSectionBytes,
            "analysis sections require 1 byte to 64 MiB of materialized data");
    require(S->AnalysisSections.size() < kMaxAnalysisSections,
            "too many analysis sections");
    size_t TotalBytes = DataSize;
    for (const auto &Record : S->AnalysisSections) {
      const auto Size = size_t(*Record.getAsObject()->getInteger("size"));
      require(Size <= kMaxAnalysisSectionBytes - TotalBytes,
              "analysis section data exceeds the 64 MiB session limit");
      TotalBytes += Size;
    }
    const auto Parsed = parseDocument(Description);
    const auto *Object = Parsed.getAsObject();
    require(Object != nullptr,
            "analysis section description must be an object");
    fields(*Object, {"schema", "name", "address", "permissions", "functions"});
    require(Object->getString("schema") == "neverd.analysis-section.v1",
            "unsupported analysis section schema");
    const auto Name = name(*Object, "name");
    const auto Start = address(*Object, "address");
    require(Start && Start < InvalidVA && DataSize <= InvalidVA - Start,
            "analysis section address range overflows");
    if (S->Img.Bits == Bitness::Bits32)
      require(Start < (uint64_t{1} << 32) &&
                  DataSize <= (uint64_t{1} << 32) - Start,
              "analysis section exceeds the image address width");
    const auto Permissions = Object->getString("permissions");
    require(Permissions && (*Permissions == "r" || *Permissions == "rw" ||
                            *Permissions == "rx" || *Permissions == "rwx"),
            "analysis section permissions must be r, rw, rx or rwx");
    SegmentFlags Flags = SegmentFlags::Readable;
    if (Permissions->contains('w'))
      Flags = Flags | SegmentFlags::Writable;
    if (Permissions->contains('x'))
      Flags = Flags | SegmentFlags::Executable;
    for (const auto &Segment : S->Img.Segments)
      require(Segment.Name != Name &&
                  !overlaps(Start, DataSize, Segment.VA, Segment.Size),
              "analysis section name or range conflicts with a mapped segment");
    for (const auto &Section : S->Img.Sections)
      require(Section.Name != Name &&
                  !overlaps(Start, DataSize, Section.VA, Section.Size),
              "analysis section name or range conflicts with a section");

    const auto *Declarations = Object->getArray("functions");
    require(!Object->get("functions") || Declarations,
            "analysis section functions must be an array");
    require(!Declarations ||
                Declarations->size() <= kMaxAnalysisSectionFunctions,
            "too many declared functions");
    size_t FunctionCount = Declarations ? Declarations->size() : 0;
    for (const auto &Record : S->AnalysisSections) {
      FunctionCount += Record.getAsObject()->getArray("functions")->size();
      require(FunctionCount <= kMaxAnalysisSectionFunctions,
              "analysis sections exceed the 4096 function session limit");
    }
    BinaryImage Prepared;
    auto &Segment = Prepared.addSegment(Name, Start, DataSize, Flags);
    Segment.Data.assign(Data, Data + DataSize);
    auto &Section = Prepared.addSection(Name, Start, DataSize, Flags);
    Section.SegmentName = Name;
    Section.Data = Segment.Data;
    llvm::json::Array Functions;
    Decoder Decoder;
    require(!Declarations || Declarations->empty() || Decoder.init(S->Img),
            "cannot initialize the image decoder");
    if (Declarations)
      for (const auto &Declaration : *Declarations) {
        const auto *Function = Declaration.getAsObject();
        require(Function != nullptr, "function declaration must be an object");
        fields(*Function, {"name", "offset", "size", "source_address"});
        const auto Offset = address(*Function, "offset");
        const auto Size = Function->getInteger("size");
        require(Size && *Size > 0 && Offset < DataSize &&
                    uint64_t(*Size) <= DataSize - Offset,
                "function extent is outside materialized section bytes");
        require(hasFlag(Flags, SegmentFlags::Executable),
                "functions require an executable analysis section");
        const auto FunctionName = name(*Function, "name");
        const auto Entry = Start + Offset;
        if (S->Img.Arch == Arch::AArch64)
          require(Entry % 4 == 0, "unaligned AArch64 function entry");
        for (const auto &Existing : S->Img.Symbols)
          require(Existing.Name != FunctionName &&
                      (!Existing.IsFunc || Existing.Addr != Entry),
                  "declared function conflicts with an existing symbol");
        for (const auto &Existing : S->Functions)
          require(Existing.Name != FunctionName,
                  "declared function conflicts with an existing display name");
        for (const auto &Existing : Prepared.Symbols)
          require(Existing.Name != FunctionName &&
                      !overlaps(Entry, uint64_t(*Size), Existing.Addr,
                                Existing.Size),
                  "analysis function names or extents conflict");
        DecodedInsn First;
        const int Width =
            Decoder.decodeOne(Data + Offset, size_t(*Size), Entry, First);
        require(
            Width > 0 && uint64_t(Width) <= uint64_t(*Size),
            "declared function does not start with a decodable instruction");
        auto &Symbol =
            Prepared.addSymbol(FunctionName, Entry, uint64_t(*Size), true);
        Symbol.Origin = NameOrigin::User;
        llvm::json::Object Record{{"name", FunctionName},
                                  {"offset", "0x" + llvm::utohexstr(Offset)},
                                  {"size", *Size}};
        if (Function->get("source_address")) {
          const auto Source = address(*Function, "source_address");
          Record["source_address"] = "0x" + llvm::utohexstr(Source);
        }
        Functions.push_back(std::move(Record));
      }
    Update Next(*S);
    Next.Segments.push_back(std::move(Prepared.Segments.front()));
    Next.Sections.push_back(std::move(Prepared.Sections.front()));
    Next.Symbols.insert(Next.Symbols.end(), Prepared.Symbols.begin(),
                        Prepared.Symbols.end());
    Next.Records.push_back(
        llvm::json::Object{{"schema", "neverd.analysis-section.v1"},
                           {"name", Name},
                           {"address", "0x" + llvm::utohexstr(Start)},
                           {"size", uint64_t(DataSize)},
                           {"permissions", Permissions->str()},
                           {"functions", std::move(Functions)}});
    Next.publish(*S);
    return 1;
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return 0;
  }
}

int neverd_analysis_section_remove(neverd_session_t Sess, const char *Name) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  try {
    requireImage(S);
    require(Name != nullptr, "analysis section name is null");
    auto Record =
        std::find_if(S->AnalysisSections.begin(), S->AnalysisSections.end(),
                     [&](const auto &Value) {
                       return Value.getAsObject()->getString("name") == Name;
                     });
    require(Record != S->AnalysisSections.end(),
            "analysis section was not found");
    const auto *Object = Record->getAsObject();
    const auto Start = address(*Object, "address");
    Update Next(*S);
    std::erase_if(Next.Segments, [&](const auto &Value) {
      return Value.Name == Name && Value.VA == Start;
    });
    std::erase_if(Next.Sections, [&](const auto &Value) {
      return Value.Name == Name && Value.VA == Start;
    });
    // Remove only declarations owned by this section. A base-image symbol may
    // have pointed into an unmapped range before the section was added.
    std::erase_if(Next.Symbols, [&](const auto &Value) {
      for (const auto &Declaration : *Object->getArray("functions")) {
        const auto &Function = *Declaration.getAsObject();
        if (Value.Addr == Start + address(Function, "offset") &&
            Value.Name == *Function.getString("name"))
          return true;
      }
      return false;
    });
    Next.Records.erase(Next.Records.begin() +
                       (Record - S->AnalysisSections.begin()));
    Next.publish(*S);
    return 1;
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return 0;
  }
}

const char *neverd_analysis_sections_json(neverd_session_t Sess,
                                          int IncludeData) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  try {
    require(S->Loaded, "no binary loaded");
    auto Records = S->AnalysisSections;
    if (IncludeData)
      for (auto &Value : Records) {
        auto &Object = *Value.getAsObject();
        const auto Start = address(Object, "address");
        const auto Size = size_t(*Object.getInteger("size"));
        const auto *Data = S->Img.readVA(Start, Size);
        require(Data != nullptr, "analysis section bytes are unavailable");
        Object["data_hex"] =
            llvm::toHex(llvm::ArrayRef<uint8_t>(Data, Size), true);
      }
    llvm::json::Object State{{"schema", "neverd.analysis-sections.v1"},
                             {"sections", std::move(Records)}};
    if (IncludeData)
      State["sample_sha256"] = sourceHash(*S);
    return dupStr(jsonToString(std::move(State)));
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return nullptr;
  }
}

int neverd_analysis_sections_import_json(neverd_session_t Sess,
                                         const char *State) {
  auto *S = toSession(Sess);
  if (!S)
    return 0;
  S->clearError();
  try {
    require(S->Loaded, "no binary loaded");
    require(State && std::strlen(State) <= kMaxAnalysisSectionStateBytes,
            "invalid analysis section state");
    const auto Parsed = parseDocument(State);
    const auto *Object = Parsed.getAsObject();
    require(Object != nullptr, "analysis section state must be an object");
    fields(*Object, {"schema", "sample_sha256", "sections"});
    require(Object->getString("schema") == "neverd.analysis-sections.v1",
            "unsupported analysis section state schema");
    const auto *Sections = Object->getArray("sections");
    require(Sections && Sections->size() <= kMaxAnalysisSections,
            "analysis section state requires a bounded sections array");
    require(Object->getString("sample_sha256") == sourceHash(*S),
            "analysis sections do not match the loaded source SHA-256");

    // Validate through the same add/remove operations on a private image.
    // A bad later section cannot remove or replace any live section/pipeline.
    Session Candidate;
    Candidate.Img = S->Img;
    Candidate.Loaded = true;
    Candidate.FilePath = S->FilePath;
    Candidate.AnalysisSections = S->AnalysisSections;
    Candidate.Renames = S->Renames;
    Candidate.Functions = S->Functions;
    for (const auto &Previous : S->AnalysisSections) {
      const auto Name = Previous.getAsObject()->getString("name")->str();
      if (!neverd_analysis_section_remove(&Candidate, Name.c_str()))
        throw std::invalid_argument(Candidate.LastError);
    }
    for (const auto &Value : *Sections) {
      const auto *Record = Value.getAsObject();
      require(Record != nullptr,
              "analysis section state entry must be an object");
      const auto Hex = Record->getString("data_hex");
      const auto Size = Record->getInteger("size");
      std::string Bytes;
      require(Hex && !Hex->empty() &&
                  Hex->size() <= 2 * kMaxAnalysisSectionBytes &&
                  Hex->size() % 2 == 0 && llvm::tryGetFromHex(*Hex, Bytes) &&
                  Size && *Size > 0 && uint64_t(*Size) == Bytes.size(),
              "analysis section state has invalid byte data or size");
      auto Description = *Record;
      Description.erase("data_hex");
      Description.erase("size");
      const auto JSON = jsonToString(std::move(Description));
      if (!neverd_analysis_section_add(
              &Candidate, JSON.c_str(),
              reinterpret_cast<const unsigned char *>(Bytes.data()),
              Bytes.size()))
        throw std::invalid_argument(Candidate.LastError);
    }
    Update Next(*S);
    Next.Segments = std::move(Candidate.Img.Segments);
    Next.Sections = std::move(Candidate.Img.Sections);
    Next.Symbols = std::move(Candidate.Img.Symbols);
    Next.Records = std::move(Candidate.AnalysisSections);
    Next.publish(*S);
    return 1;
  } catch (const std::exception &Error) {
    S->setError(Error.what());
    return 0;
  }
}
