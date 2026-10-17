//===- COFFSourceImage.cpp - Compiler-produced PE replacement images ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFSourceImage.h"

#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFLayoutGrowth.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/object/PELayout.h"

#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <array>
#include <map>
#include <set>

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "PE source image: " + Message);
}
using RuntimeRow = COFFRuntimeFunctionRecord;

llvm::Expected<llvm::ArrayRef<uint8_t>>
directoryBytes(const BinaryImage &Original, const PEHeaderPtrs &PE,
               uint32_t Kind) {
  auto *Directory = getPEDataDirectory(PE, Kind);
  if (!Directory || (!Directory->Size && !Directory->RelativeVirtualAddress))
    return llvm::ArrayRef<uint8_t>();
  if (!Directory->Size || !Directory->RelativeVirtualAddress)
    return fail("inconsistent original data directory");
  const auto *Data = Original.readVA(
      Original.Base + Directory->RelativeVirtualAddress, Directory->Size);
  if (!Data)
    return fail("original data directory is not file-backed");
  return llvm::ArrayRef<uint8_t>(Data, Directory->Size);
}

bool owns(const CompiledImage &Image, uint64_t VA, uint64_t Size,
          llvm::mc_rewrite::RewriteSectionKind Kind) {
  return llvm::any_of(Image.Sections, [&](const auto &Section) {
    return Section.IsAllocated && Section.IsInImage && Section.Kind == Kind &&
           VA >= Section.VA &&
           rangeInBounds(VA - Section.VA, Size, Section.Size);
  });
}

llvm::Error verifyLanguage(const BinaryImage &Reloaded,
                           const BinaryImage &Original,
                           const CompiledImage &Compiled,
                           const std::map<uint32_t, RuntimeRow> &Generated,
                           bool NativeObject) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  std::map<uint64_t, const llvm::mc_rewrite::RewriteFunctionRange *> Ranges;
  for (const auto &Range : Compiled.FunctionRanges)
    if (!Ranges.emplace(Range.BeginVA, &Range).second)
      return fail("ambiguous compiler function range");
  std::set<uint32_t> Seen;
  for (const auto &EH : Reloaded.ExceptionMetadata.Functions) {
    if (EH.CodeRange.Begin < Original.Base ||
        EH.CodeRange.Begin - Original.Base > UINT32_MAX)
      continue;
    auto Row = Generated.find(
        static_cast<uint32_t>(EH.CodeRange.Begin - Original.Base));
    if (Row == Generated.end())
      continue;
    if (!Seen.insert(Row->first).second ||
        EH.CodeRange.End != Original.Base + Row->second.End ||
        (Original.Arch == Arch::X64 &&
         EH.UnwindInfoRVA != Row->second.Unwind) ||
        EH.ParseStatus != ExceptionParseStatus::Complete ||
        EH.Kind != RuntimeFunctionKind::Primary)
      return fail("generated unwind record did not reload exactly");
    if (Original.Arch != Arch::X64) {
      if (EH.PackedUnwindData != Row->second.Unwind ||
          (EH.Personality != ExceptionPersonality::None &&
           Original.Arch != Arch::AArch64))
        return fail("ARM compiler unwind record did not reload exactly");
    }
    const auto *Range = NativeObject ? nullptr : Ranges.at(EH.CodeRange.Begin);
    if (Original.Arch == Arch::X64) {
      const auto *Unwind = Reloaded.readVA(EH.UnwindInfoVA, 4);
      if (!Unwind ||
          !owns(Compiled, EH.UnwindInfoVA,
                4 + 2 * llvm::alignTo(uint64_t(Unwind[2]), 2) +
                    (EH.Personality == ExceptionPersonality::None ? 0 : 8),
                Kind::ReadOnlyData))
        return fail("unwind program leaves compiler read-only storage");
    } else if (!(EH.PackedUnwindData & 3) &&
               (EH.NativeUnwindBytes.empty() ||
                !owns(Compiled, EH.UnwindInfoVA, EH.NativeUnwindBytes.size(),
                      Kind::ReadOnlyData))) {
      return fail("ARM xdata leaves compiler read-only storage");
    }
    if (EH.Personality == ExceptionPersonality::None)
      continue;
    if (NativeObject)
      return fail("native object language-handler ownership is unsupported");
    const auto Handler =
        findCOFFExceptionPersonalityVA(Original, "__CxxFrameHandler3");
    if (!Handler || EH.PersonalityVA != *Handler ||
        EH.Personality != ExceptionPersonality::CxxFrameHandler3 || !EH.Cxx ||
        EH.Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH3 ||
        !EH.Cxx->hasValidStateGraph() || EH.Cxx->hasExceptionSpecification() ||
        EH.Cxx->HasDynamicStackAlignment || EH.Cxx->BBTFlags ||
        !owns(Compiled, EH.HandlerDataVA, 4, Kind::ReadOnlyData) ||
        !owns(Compiled, EH.Cxx->NativeFuncInfoVA, 40, Kind::ReadOnlyData))
      return fail("unsupported or incomplete compiler C++ FH3 metadata");
    const auto &Cxx = *EH.Cxx;
    const uint64_t Parent =
        Range->ParentOwnerVA ? Range->ParentOwnerVA : Range->OwnerVA;
    auto IsGroupCode = [&](uint64_t VA, bool AllowEnd) {
      return llvm::any_of(Compiled.FunctionRanges, [&](const auto &Candidate) {
        return (Candidate.OwnerVA == Parent ||
                Candidate.ParentOwnerVA == Parent) &&
               VA >= Candidate.BeginVA &&
               (VA < Candidate.EndVA || (AllowEnd && VA == Candidate.EndVA));
      });
    };
    auto IsGroupEntry = [&](uint64_t VA) {
      auto It = Ranges.find(VA);
      return It != Ranges.end() && (It->second->OwnerVA == Parent ||
                                    It->second->ParentOwnerVA == Parent);
    };
    for (const auto &IP : Cxx.IPMap)
      if (!IsGroupCode(IP.IP, true))
        return fail("C++ IP state leaves its compiler function group");
    for (const auto &Action : Cxx.UnwindMap)
      if (Action.ActionVA && !IsGroupEntry(Action.ActionVA))
        return fail("C++ cleanup does not name its compiler funclet");
    for (const auto &Try : Cxx.TryBlocks)
      for (const auto &Catch : Try.Handlers) {
        auto Target = Ranges.find(Catch.HandlerVA);
        if (Target == Ranges.end() || Target->second->ParentOwnerVA != Parent ||
            Catch.ParentFrameOffset < 0 || Catch.ParentFrameOffset % 8 ||
            (Original.Arch == Arch::AArch64 && Catch.ParentFrameOffset != 0) ||
            (Catch.TypeDescriptorVA &&
             !Reloaded.readVA(Catch.TypeDescriptorVA, 17)))
          return fail("C++ catch has no exact compiler funclet/type record");
        const auto *CatchEH =
            Reloaded.ExceptionMetadata.findFunction(Catch.HandlerVA);
        if (!CatchEH || !CatchEH->Cxx ||
            CatchEH->Cxx->NativeFuncInfoVA != Cxx.NativeFuncInfoVA)
          return fail("C++ catch changes its parent FuncInfo identity");
      }
    // Bound each physical FH3 table to compiler-produced read-only
    // storage.
    const auto *Info = Reloaded.readVA(Cxx.NativeFuncInfoVA, 40);
    for (const auto [CountOffset, PointerOffset, Stride] :
         {std::array<unsigned, 3>{4, 8, 8}, {12, 16, 20}, {20, 24, 8}}) {
      const uint32_t Count = readLE<uint32_t>(Info + CountOffset);
      const uint32_t RVA = readLE<uint32_t>(Info + PointerOffset);
      if (Count && !owns(Compiled, Original.Base + RVA,
                         uint64_t(Count) * Stride, Kind::ReadOnlyData))
        return fail("C++ state table leaves compiler read-only storage");
      if (Stride == 20)
        for (uint32_t I = 0; I < Count; ++I) {
          const auto *Try =
              Reloaded.readVA(Original.Base + RVA + uint64_t(I) * 20, 20);
          const auto NumCatches = readLE<uint32_t>(Try + 12);
          const auto Handlers = readLE<uint32_t>(Try + 16);
          if (!NumCatches ||
              !owns(Compiled, Original.Base + Handlers,
                    uint64_t(NumCatches) * 20, Kind::ReadOnlyData))
            return fail("C++ handler table leaves compiler read-only storage");
        }
    }
  }
  if (Seen.size() != Generated.size())
    return fail("generated runtime-function "
                "directory is incomplete");
  return llvm::Error::success();
}
} // namespace

static llvm::Error installCOFFImage(std::vector<uint8_t> &Binary,
                                    const BinaryImage &Original,
                                    const CompiledImage &Compiled,
                                    const COFFObjectEvidence *Object,
                                    bool OriginalCallbacks) {
  using Kind = llvm::mc_rewrite::RewriteSectionKind;
  if (auto E = validateCOFFReplacementFeatures(Original, Compiled.TargetArch))
    return E;
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  if (!PE.valid() || !PE.SectionTable ||
      PE.Is64 !=
          (Original.Arch == Arch::X64 || Original.Arch == Arch::AArch64) ||
      Compiled.TargetArch != Original.Arch ||
      (Object
           ? ((!OriginalCallbacks &&
               Compiled.InputKind != RewriteInputKind::ReplacementObject) ||
              !Compiled.FunctionRanges.empty() ||
              !Compiled.FunctionOwnerAddrs.empty() || Object->Functions.empty())
           : (Compiled.InputKind != RewriteInputKind::ReplacementIR ||
              !Compiled.FunctionRangesValid ||
              !llvm::mc_rewrite::validateRewriteFunctionRanges(
                  Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))))
    return fail(
        "matching PE headers and authenticated compiler ranges are required");
  const uint64_t Alignment = getPESectionAlignment(PE);
  const uint64_t FileAlignment = getPEFileAlignment(PE);
  if (!llvm::isPowerOf2_64(Alignment) || Alignment < 4096 ||
      Alignment > 65536 || !llvm::isPowerOf2_64(FileAlignment) ||
      FileAlignment < 512 || FileAlignment > Alignment ||
      getPEImageBase(PE) != Original.Base || Compiled.BaseVA < Original.Base ||
      Compiled.BaseVA !=
          Original.Base + llvm::alignTo(getPESizeOfImage(PE), Alignment))
    return fail("invalid PE placement or native alignment");
  auto Storage = validatePatchStorageLayout(Compiled, Alignment);
  if (!Storage)
    return Storage.takeError();
  auto OldPData = directoryBytes(Original, PE, llvm::COFF::EXCEPTION_TABLE);
  if (!OldPData)
    return OldPData.takeError();
  std::map<uint32_t, RuntimeRow> Rows, Generated;
  const unsigned RowSize = Original.Arch == Arch::X64 ? 12 : 8;
  auto AddRows = [&](llvm::ArrayRef<uint8_t> Bytes, bool New) -> llvm::Error {
    if (Original.Arch == Arch::X86) {
      if (!Bytes.empty())
        return fail(
            "x86 uses compiler registration code, not a pdata directory");
      return llvm::Error::success();
    }
    if (Bytes.size() % RowSize)
      return fail("misaligned runtime-function table");
    auto Read = [&](uint32_t RVA, uint64_t Size) -> const uint8_t * {
      const uint64_t VA = Original.Base + RVA;
      if (!New)
        return Original.readVA(VA, Size);
      if (VA < Compiled.BaseVA ||
          !rangeInBounds(VA - Compiled.BaseVA, Size, Compiled.Bytes.size()))
        return nullptr;
      return Compiled.Bytes.data() + VA - Compiled.BaseVA;
    };
    for (size_t I = 0; I < Bytes.size(); I += RowSize) {
      auto Parsed = decodeCOFFRuntimeFunction(Bytes.slice(I, RowSize),
                                              Original.Arch, Read);
      if (!Parsed)
        return Parsed.takeError();
      const RuntimeRow Row = *Parsed;
      if (!Row.Begin || Row.Begin >= Row.End || !Row.Unwind ||
          !Rows.emplace(Row.Begin, Row).second)
        return fail("invalid or duplicate runtime-function entry");
      if (!New)
        continue;
      const uint64_t Begin = Original.Base + Row.Begin,
                     End = Original.Base + Row.End;
      const auto Count =
          Object
              ? llvm::count_if(Object->Functions,
                               [&](const auto &Range) {
                                 return Range.BeginVA == Begin &&
                                        Range.EndVA == End;
                               })
              : llvm::count_if(Compiled.FunctionRanges, [&](const auto &Range) {
                  return Range.BeginVA == Begin && Range.EndVA == End;
                });
      if (Count != 1 || !owns(Compiled, Begin, End - Begin, Kind::Code) ||
          ((Original.Arch == Arch::X64 || !(Row.Unwind & 3)) &&
           !owns(Compiled, Original.Base + Row.Unwind, 4, Kind::ReadOnlyData)))
        return fail("runtime row disagrees with its exact compiler range");
      if (Row.HasLanguageHandler &&
          (Object ||
           (Original.Arch != Arch::X64 && Original.Arch != Arch::AArch64)))
        return fail("unverified replacement ARM language-handler program");
      Generated.emplace(Row.Begin, Row);
    }
    return llvm::Error::success();
  };
  if (auto Error = AddRows(*OldPData, false))
    return Error;
  std::map<uint32_t, std::set<uint16_t>> NewRelocs;
  std::map<uint64_t, unsigned> RelocatedFields;
  if (Object) {
    uint64_t PreviousEnd = 0;
    for (const auto &Range : Object->Functions) {
      if (!Range.BeginVA || Range.BeginVA >= Range.EndVA ||
          Range.BeginVA < PreviousEnd ||
          !owns(Compiled, Range.BeginVA, Range.EndVA - Range.BeginVA,
                Kind::Code))
        return fail("invalid native object function extent");
      PreviousEnd = Range.EndVA;
    }
    for (const auto &Field : Object->BaseRelocations) {
      const bool ValidType =
          (PE.Is64 && Field.Type == llvm::COFF::IMAGE_REL_BASED_DIR64 &&
           Field.Width == 8) ||
          (!PE.Is64 && Field.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW &&
           Field.Width == 4) ||
          (Original.Arch == Arch::ARM &&
           Field.Type == llvm::COFF::IMAGE_REL_BASED_ARM_MOV32T &&
           Field.Width == 8);
      if (!ValidType || Field.VA < Compiled.BaseVA ||
          Field.VA < Original.Base ||
          !rangeInBounds(Field.VA - Compiled.BaseVA, Field.Width,
                         Compiled.Bytes.size()) ||
          Field.VA - Original.Base > UINT32_MAX - (Field.Width - 1))
        return fail("invalid native object absolute relocation");
      const uint64_t RVA = Field.VA - Original.Base;
      auto Next = RelocatedFields.lower_bound(RVA);
      if ((Next != RelocatedFields.end() && Next->first - RVA < Field.Width) ||
          (Next != RelocatedFields.begin() &&
           RVA - std::prev(Next)->first < std::prev(Next)->second))
        return fail("overlapping native object absolute relocations");
      RelocatedFields.emplace(RVA, Field.Width);
      NewRelocs[uint32_t(RVA & ~4095ULL)].insert(
          uint16_t((Field.Type << 12) | (RVA & 4095)));
    }
  }
  for (const auto &Section : Compiled.Sections) {
    if (!Section.IsAllocated || !Section.IsInImage || !Section.Size)
      continue;
    if (llvm::StringRef(Section.Name).starts_with(".pdata"))
      if (auto Error = AddRows(llvm::ArrayRef<uint8_t>(Compiled.Bytes)
                                   .slice(Section.Offset, Section.Size),
                               true))
        return Error;
    std::set<uint64_t> PairedHighFields;
    for (const auto &Fixup : Section.FixupReferences) {
      if (Fixup.IsPCRel || !Fixup.SubtractSymbol.empty() || Fixup.Specifier)
        continue;
      unsigned Width = 0, Type = 0;
      if (Original.Arch == Arch::ARM &&
          Fixup.KindName == "fixup_t2_movw_lo16") {
        auto High =
            llvm::find_if(Section.FixupReferences, [&](const auto &Other) {
              return Other.Offset == Fixup.Offset + 4 &&
                     Other.KindName == "fixup_t2_movt_hi16" &&
                     Other.Symbol == Fixup.Symbol &&
                     Other.Addend == Fixup.Addend &&
                     Other.SubtractSymbol.empty() && Other.Specifier == 0 &&
                     Other.IsResolved && !Other.IsPCRel;
            });
        if (High == Section.FixupReferences.end() ||
            High->ResolvedValue != Fixup.ResolvedValue ||
            Section.Kind != Kind::Code ||
            !PairedHighFields.insert(High->Offset).second)
          return fail("ARM absolute address has no exact MOVW/MOVT pair");
        Width = 8;
        Type = llvm::COFF::IMAGE_REL_BASED_ARM_MOV32T;
      } else if (Original.Arch == Arch::ARM &&
                 Fixup.KindName == "fixup_t2_movt_hi16") {
        // The complete pair is registered at MOVW, independently of callback
        // order. Check all high fields after the collection pass.
        continue;
      } else if (Fixup.Kind == llvm::FK_Data_8 && Fixup.BitWidth == 64 &&
                 PE.Is64) {
        Width = 8;
        Type = llvm::COFF::IMAGE_REL_BASED_DIR64;
      } else if ((Fixup.Kind == llvm::FK_Data_4 ||
                  (Original.Arch == Arch::X86 &&
                   Fixup.KindName == "reloc_signed_4byte")) &&
                 Fixup.BitWidth == 32 && !PE.Is64) {
        Width = 4;
        Type = llvm::COFF::IMAGE_REL_BASED_HIGHLOW;
      } else {
        continue; // Other ISA fixups encode offsets/RVAs, not pointer fields.
      }
      if (!Fixup.IsResolved ||
          !rangeInBounds(Fixup.Offset, Width, Section.Size))
        return fail("absolute address lacks bounded relocation storage");
      const uint64_t RVA = Section.VA - Original.Base + Fixup.Offset;
      if (RVA > UINT32_MAX - (Width - 1))
        return fail("absolute relocation exceeds PE limits");
      auto Next = RelocatedFields.lower_bound(RVA);
      if ((Next != RelocatedFields.end() && Next->first - RVA < Width) ||
          (Next != RelocatedFields.begin() &&
           RVA - std::prev(Next)->first < std::prev(Next)->second))
        return fail("overlapping absolute relocation fields");
      RelocatedFields.emplace(RVA, Width);
      if (!NewRelocs[static_cast<uint32_t>(RVA & ~4095ULL)]
               .insert(static_cast<uint16_t>((Type << 12) | (RVA & 4095)))
               .second)
        return fail("duplicate absolute relocation");
    }
    if (Original.Arch == Arch::ARM)
      for (const auto &Fixup : Section.FixupReferences)
        if (Fixup.KindName == "fixup_t2_movt_hi16" && !Fixup.IsPCRel &&
            !Fixup.Specifier && !PairedHighFields.count(Fixup.Offset))
          return fail("unpaired ARM absolute MOVT address");
  }

  uint64_t PreviousEnd = 0;
  std::vector<uint8_t> Metadata;
  for (const auto &[Begin, Row] : Rows) {
    if (Begin < PreviousEnd)
      return fail("overlapping runtime-function ranges");
    PreviousEnd = Row.End;
    for (unsigned I = 0; I < Row.WordCount; ++I) {
      const auto Offset = Metadata.size();
      Metadata.resize(Offset + 4);
      writeLE<uint32_t>(Metadata.data() + Offset, Row.Words[I]);
    }
  }
  const uint64_t EHSize = Metadata.size();
  const uint64_t MetaRVA = llvm::alignTo(
      Compiled.BaseVA - Original.Base + Compiled.Bytes.size(), Alignment);
  uint64_t RelocOffset = 0, RelocSize = 0;
  if (!NewRelocs.empty()) {
    if (PE.FileHeader->Characteristics & llvm::COFF::IMAGE_FILE_RELOCS_STRIPPED)
      return fail("absolute pointers require a relocatable original image");
    auto OldRelocs =
        directoryBytes(Original, PE, llvm::COFF::BASE_RELOCATION_TABLE);
    if (!OldRelocs)
      return OldRelocs.takeError();
    for (size_t Offset = 0; Offset < OldRelocs->size();) {
      if (!rangeInBounds(Offset, 8, OldRelocs->size()))
        return fail("truncated original base-relocation block");
      const auto *Block = OldRelocs->data() + Offset;
      const uint32_t Page = readLE<uint32_t>(Block);
      const uint32_t Size = readLE<uint32_t>(Block + 4);
      if ((Page & 4095) || Size < 8 || (Size & 3) ||
          !rangeInBounds(Offset, Size, OldRelocs->size()))
        return fail("invalid original base-relocation block");
      for (uint32_t I = 8; I < Size; I += 2) {
        const uint16_t Entry = readLE<uint16_t>(Block + I);
        const uint16_t Type = Entry >> 12;
        if (Type == llvm::COFF::IMAGE_REL_BASED_ABSOLUTE)
          continue;
        const unsigned Width = Type == llvm::COFF::IMAGE_REL_BASED_DIR64     ? 8
                               : Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ? 4
                               : Type == llvm::COFF::IMAGE_REL_BASED_ARM_MOV32T
                                   ? 8
                                   : 0;
        if (!Width || (PE.Is64 && Type != llvm::COFF::IMAGE_REL_BASED_DIR64) ||
            (!PE.Is64 && Type == llvm::COFF::IMAGE_REL_BASED_DIR64) ||
            (Type == llvm::COFF::IMAGE_REL_BASED_ARM_MOV32T &&
             Original.Arch != Arch::ARM) ||
            uint64_t(Page) + (Entry & 4095) + Width > getPESizeOfImage(PE))
          return fail("unsupported original base relocation");
      }
      Offset += Size;
    }
    RelocOffset = Metadata.size();
    Metadata.insert(Metadata.end(), OldRelocs->begin(), OldRelocs->end());
    for (const auto &[Page, Entries] : NewRelocs) {
      const uint32_t Size =
          static_cast<uint32_t>(llvm::alignTo(8 + 2 * Entries.size(), 4));
      const auto Offset = Metadata.size();
      Metadata.resize(Offset + Size, 0);
      writeLE<uint32_t>(Metadata.data() + Offset, Page);
      writeLE<uint32_t>(Metadata.data() + Offset + 4, Size);
      size_t Index = Offset + 8;
      for (uint16_t Entry : Entries) {
        writeLE<uint16_t>(Metadata.data() + Index, Entry);
        Index += 2;
      }
    }
    RelocSize = Metadata.size() - RelocOffset;
  }
  struct Chunk {
    llvm::StringRef Name;
    uint64_t RVA;
    llvm::ArrayRef<uint8_t> Bytes;
    uint32_t Flags;
  };
  const uint32_t Read = llvm::COFF::IMAGE_SCN_MEM_READ;
  const uint32_t Data = llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
  const auto Bytes = llvm::ArrayRef<uint8_t>(Compiled.Bytes);
  std::vector<Chunk> Chunks{{".ndtext", Compiled.BaseVA - Original.Base,
                             Storage->WritableOffset
                                 ? Bytes.take_front(Storage->WritableOffset)
                                 : Bytes,
                             Read | llvm::COFF::IMAGE_SCN_MEM_EXECUTE |
                                 llvm::COFF::IMAGE_SCN_CNT_CODE}};
  if (Storage->WritableOffset)
    Chunks.push_back({".nddata",
                      Compiled.BaseVA - Original.Base + Storage->WritableOffset,
                      Bytes.drop_front(Storage->WritableOffset),
                      Read | Data | llvm::COFF::IMAGE_SCN_MEM_WRITE});
  if (!Metadata.empty())
    Chunks.push_back({".ndmeta", MetaRVA, Metadata, Read | Data});
  auto HeaderGrowth = ensureCOFFSectionHeaderCapacity(Binary, Chunks.size());
  if (!HeaderGrowth)
    return HeaderGrowth.takeError();
  PE = locatePEHeaders(Binary.data(), Binary.size());
  const uint64_t TableOffset = PE.SectionTable - Binary.data();
  const uint16_t OldCount = PE.NumSections;
  size_t Index = 0;
  uint64_t ImageEnd = 0;
  uint64_t AddedCode = 0, AddedData = 0;
  for (const auto &Chunk : Chunks) {
    const uint64_t RawOffset = llvm::alignTo(Binary.size(), FileAlignment);
    const uint64_t RawSize = llvm::alignTo(Chunk.Bytes.size(), FileAlignment);
    ImageEnd = llvm::alignTo(Chunk.RVA + Chunk.Bytes.size(), Alignment);
    if (Chunk.Bytes.empty() || RawOffset + RawSize > UINT32_MAX ||
        ImageEnd > UINT32_MAX)
      return fail("new PE section exceeds format limits");
    Binary.resize(RawOffset + RawSize, 0);
    std::copy(Chunk.Bytes.begin(), Chunk.Bytes.end(),
              Binary.begin() + RawOffset);
    llvm::object::coff_section Section{};
    std::memcpy(Section.Name, Chunk.Name.data(), Chunk.Name.size());
    Section.VirtualAddress = Chunk.RVA;
    Section.VirtualSize = Chunk.Bytes.size();
    Section.PointerToRawData = RawOffset;
    Section.SizeOfRawData = RawSize;
    Section.Characteristics = Chunk.Flags;
    if (Chunk.Flags & llvm::COFF::IMAGE_SCN_CNT_CODE)
      AddedCode += RawSize;
    else
      AddedData += RawSize;
    std::memcpy(Binary.data() + TableOffset +
                    (OldCount + Index++) * sizeof(Section),
                &Section, sizeof(Section));
  }
  PE = locatePEHeaders(Binary.data(), Binary.size());
  PE.FileHeader->NumberOfSections = OldCount + Chunks.size();
  auto AddStorageTotals = [&](auto *Optional) -> llvm::Error {
    if (AddedCode > UINT32_MAX - Optional->SizeOfCode ||
        AddedData > UINT32_MAX - Optional->SizeOfInitializedData)
      return fail("PE optional-header storage totals overflow");
    Optional->SizeOfCode = Optional->SizeOfCode + AddedCode;
    Optional->SizeOfInitializedData =
        Optional->SizeOfInitializedData + AddedData;
    return llvm::Error::success();
  };
  auto Totals =
      PE.Is64
          ? AddStorageTotals(reinterpret_cast<llvm::object::pe32plus_header *>(
                PE.OptionalHeader))
          : AddStorageTotals(reinterpret_cast<llvm::object::pe32_header *>(
                PE.OptionalHeader));
  if (Totals)
    return Totals;
  setPESizeOfImage(PE, ImageEnd);
  clearPEChecksum(PE);
  clearPEDataDirectory(PE, llvm::COFF::CERTIFICATE_TABLE);
  if (EHSize)
    setPEDataDirectory(PE, llvm::COFF::EXCEPTION_TABLE, MetaRVA, EHSize);
  if (RelocSize)
    setPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE,
                       MetaRVA + RelocOffset, RelocSize);
  for (const auto [Kind, RVA, Size] :
       {std::array<uint64_t, 3>{llvm::COFF::EXCEPTION_TABLE, MetaRVA, EHSize},
        {llvm::COFF::BASE_RELOCATION_TABLE, MetaRVA + RelocOffset,
         RelocSize}}) {
    if (!Size)
      continue;
    const auto *Directory = getPEDataDirectory(PE, Kind);
    if (!Directory || Directory->RelativeVirtualAddress != RVA ||
        Directory->Size != Size)
      return fail("PE header cannot publish the complete metadata directories");
  }
  // The common validator checks the PE and OS unwind encodings. Source-native
  // ownership is checked separately, since the new directory is read-only
  // metadata and the new C++ program is not a lifted copy of the original.
  if (auto Error = validatePatchedCOFFImage(Binary, Original.Arch))
    return Error;
  COFFLoader Loader;
  auto Reloaded = Loader.loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Binary.data()),
                      Binary.size()),
      "source-candidate"));
  if (!Reloaded)
    return Reloaded.takeError();
  for (const auto &Chunk : Chunks) {
    const auto *Data =
        Reloaded->readVA(Original.Base + Chunk.RVA, Chunk.Bytes.size());
    const auto *Segment = Reloaded->getSegmentFor(Original.Base + Chunk.RVA);
    if (!Data || !Segment ||
        Segment->isWritable() !=
            bool(Chunk.Flags & llvm::COFF::IMAGE_SCN_MEM_WRITE) ||
        Segment->isExecutable() !=
            bool(Chunk.Flags & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) ||
        !std::equal(Chunk.Bytes.begin(), Chunk.Bytes.end(), Data))
      return fail("installed bytes or memory permissions disagree with the "
                  "compiler image");
  }
  return verifyLanguage(*Reloaded, Original, Compiled, Generated,
                        Object != nullptr);
}
llvm::Error installCOFFSourceImage(std::vector<uint8_t> &Binary,
                                   const BinaryImage &Original,
                                   const CompiledImage &Compiled,
                                   const COFFObjectEvidence *Object) {
  return installCOFFImage(Binary, Original, Compiled, Object, false);
}

llvm::Error
installCOFFCallbackImage(std::vector<uint8_t> &Binary,
                         const BinaryImage &Original,
                         const CompiledImage &Storage,
                         llvm::ArrayRef<COFFOriginalCallback> Callbacks) {
  // The shared writer consumes exact physical ranges, but their provenance
  // here is an authenticated original instruction prefix, never a fabricated
  // compiler or native-object identity.
  COFFObjectEvidence Ranges;
  for (const auto &Callback : Callbacks) {
    const auto &Prefix = Callback.Prefix;
    const auto *Function =
        Original.ExceptionMetadata.findFunction(Prefix.OriginalVA);
    const auto *Bytes = Original.readVA(Prefix.OriginalVA, Prefix.CopiedSize);
    if ((!Function && Original.Arch != Arch::X86) || !Bytes)
      return fail("callback has no original native frame or code bytes");
    auto Reencoded = relocateSourceCallbackPrefix(
        Original.Arch, Prefix.Mode, {Bytes, Prefix.CopiedSize},
        Prefix.OriginalVA, Prefix.CallbackVA);
    if (!Reencoded)
      return Reencoded.takeError();
    if (Reencoded->Bytes != Prefix.Bytes ||
        Reencoded->CopiedSize != Prefix.CopiedSize)
      return fail(
          "callback relocation receipt is not derived from the original bytes");
    llvm::Expected<std::vector<uint8_t>> Frame =
        Original.Arch == Arch::X86
            ? llvm::Expected<std::vector<uint8_t>>(std::vector<uint8_t>{})
            : encodeSourceCallbackCOFFUnwind(Prefix, *Function, Original.Arch);
    if (!Frame)
      return Frame.takeError();
    auto Matches = [&](uint64_t VA, llvm::ArrayRef<uint8_t> Bytes) {
      return VA >= Storage.BaseVA &&
             rangeInBounds(VA - Storage.BaseVA, Bytes.size(),
                           Storage.Bytes.size()) &&
             std::equal(Bytes.begin(), Bytes.end(),
                        Storage.Bytes.begin() + VA - Storage.BaseVA);
    };
    if (!Matches(Prefix.CallbackVA, Prefix.Bytes) ||
        (!Frame->empty() && !Matches(Callback.UnwindVA, *Frame)))
      return fail(
          "callback image differs from the decoded original frame contract");
    Ranges.Functions.push_back(
        {Prefix.CallbackVA, Prefix.CallbackVA + Prefix.Bytes.size()});
  }
  if (auto Error = installCOFFImage(Binary, Original, Storage, &Ranges, true))
    return Error;
  if (Original.Arch != Arch::X86)
    return llvm::Error::success();
  // x86 has no pdata directory. Publish ordinary typed COFF symbols for the
  // generated entry points, preserving every old symbol and string offset.
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  const uint64_t OldOffset = PE.FileHeader->PointerToSymbolTable;
  const uint64_t OldCount = PE.FileHeader->NumberOfSymbols;
  const uint64_t OldSize = OldCount * sizeof(llvm::object::coff_symbol16);
  if ((!OldOffset != !OldCount) ||
      (OldCount && !rangeInBounds(OldOffset, OldSize + 4, Binary.size())))
    return fail("invalid original COFF symbol table");
  std::vector<uint8_t> Symbols, Strings(4, 0);
  if (OldCount) {
    const uint64_t StringsOffset = OldOffset + OldSize;
    const uint32_t StringsSize =
        readLE<uint32_t>(Binary.data() + StringsOffset);
    if (StringsSize < 4 ||
        !rangeInBounds(StringsOffset, StringsSize, Binary.size()))
      return fail("invalid original COFF string table");
    Symbols.assign(Binary.begin() + OldOffset, Binary.begin() + StringsOffset);
    Strings.assign(Binary.begin() + StringsOffset,
                   Binary.begin() + StringsOffset + StringsSize);
  }
  for (const auto &Callback : Callbacks) {
    const auto &Prefix = Callback.Prefix;
    llvm::object::coff_symbol16 Symbol{};
    unsigned Owners = 0;
    forEachPESection(PE, [&](const PESectionFields &Section, uint16_t Index) {
      const uint64_t VA = Original.Base + Section.VirtualAddress;
      if (Prefix.CallbackVA >= VA &&
          rangeInBounds(Prefix.CallbackVA - VA, Prefix.Bytes.size(),
                        Section.VirtualSize)) {
        ++Owners;
        Symbol.SectionNumber = Index + 1;
        Symbol.Value = Prefix.CallbackVA - VA;
      }
    });
    if (Owners != 1)
      return fail("x86 callback symbol has no unique section");
    const std::string Name =
        "__nd_original_" + std::to_string(Prefix.CallbackVA);
    if (Strings.size() > UINT32_MAX - Name.size() - 1)
      return fail("callback string table exceeds COFF limits");
    Symbol.Name.Offset.Zeroes = 0;
    Symbol.Name.Offset.Offset = Strings.size();
    Symbol.Type = llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION
                  << llvm::COFF::SCT_COMPLEX_TYPE_SHIFT;
    Symbol.StorageClass = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
    const auto *Data = reinterpret_cast<const uint8_t *>(&Symbol);
    Symbols.insert(Symbols.end(), Data, Data + sizeof(Symbol));
    Strings.insert(Strings.end(), Name.begin(), Name.end());
    Strings.push_back(0);
  }
  writeLE<uint32_t>(Strings.data(), Strings.size());
  const uint64_t NewOffset = Binary.size();
  if (Symbols.size() / sizeof(llvm::object::coff_symbol16) > UINT32_MAX ||
      NewOffset > UINT32_MAX ||
      Symbols.size() + Strings.size() > UINT32_MAX - NewOffset)
    return fail("callback symbol table exceeds COFF limits");
  Binary.insert(Binary.end(), Symbols.begin(), Symbols.end());
  Binary.insert(Binary.end(), Strings.begin(), Strings.end());
  PE = locatePEHeaders(Binary.data(), Binary.size());
  PE.FileHeader->PointerToSymbolTable = NewOffset;
  PE.FileHeader->NumberOfSymbols =
      Symbols.size() / sizeof(llvm::object::coff_symbol16);
  return llvm::Error::success();
}
} // namespace neverd
