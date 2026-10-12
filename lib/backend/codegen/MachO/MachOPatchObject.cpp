//===- MachOPatchObject.cpp - Native Mach-O replacement installation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/Object/MachO.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {
using Kind = llvm::mc_rewrite::RewriteSectionKind;
using namespace llvm::MachO;
constexpr uint64_t MaxStorage = 64 * 1024 * 1024;
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "Mach-O patch object: " + Message);
}
std::string canonical(llvm::StringRef Name) {
  return (Name.starts_with("_") ? Name.drop_front() : Name).str();
}
struct Symbol {
  std::string Name;
  uint32_t Section = 0;
  uint64_t Offset = 0, Size = 0;
  bool Function = false, Thumb = false;
};
struct Relocation {
  size_t Section;
  uint64_t Offset;
  unsigned Type, Width, SymbolIndex;
  bool External, PCRel, Scattered;
  uint64_t ScatteredVA = 0;
  std::optional<uint64_t> SubtractVA;
  std::optional<unsigned> SubtractSymbol;
  unsigned HalfLength = 0, OtherHalf = 0;
};
struct NativeMachO {
  CompiledImage Image;
  std::map<uint32_t, size_t> Sections;
  std::map<uint32_t, uint64_t> OldSectionVAs;
  std::map<uint32_t, Symbol> Symbols;
  std::map<std::string, uint32_t> Names;
  std::vector<Relocation> Relocations;
  std::vector<NativeMachOCompactRange> Compact;
  std::vector<DwarfEHFrameRecord> OldFDEs;
  std::optional<size_t> EH, CompactSection;
  bool SjLj = false;
};

llvm::Expected<NativeMachO> readObject(llvm::StringRef Bytes,
                                       const BinaryImage &Original) {
  auto File = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(Bytes, "patch-object"));
  if (!File)
    return File.takeError();
  auto *M = llvm::dyn_cast<llvm::object::MachOObjectFile>(File->get());
  if (!M || !M->isLittleEndian() ||
      M->getBytesInAddress() != Original.getPointerSize())
    return fail("expected a matching little-endian native Mach-O object");
  const uint32_t CPU =
      M->is64Bit() ? M->getHeader64().cputype : M->getHeader().cputype;
  const uint32_t Subtype =
      (M->is64Bit() ? M->getHeader64().cpusubtype : M->getHeader().cpusubtype) &
      ~CPU_SUBTYPE_MASK;
  const uint32_t Filetype =
      M->is64Bit() ? M->getHeader64().filetype : M->getHeader().filetype;
  const uint32_t Flags =
      M->is64Bit() ? M->getHeader64().flags : M->getHeader().flags;
  const uint32_t ExpectedCPU = Original.Arch == Arch::X86   ? CPU_TYPE_X86
                               : Original.Arch == Arch::X64 ? CPU_TYPE_X86_64
                               : Original.Arch == Arch::ARM ? CPU_TYPE_ARM
                               : Original.Arch == Arch::AArch64 ? CPU_TYPE_ARM64
                                                                : 0;
  if (Filetype != MH_OBJECT || CPU != ExpectedCPU ||
      !(Flags & MH_SUBSECTIONS_VIA_SYMBOLS))
    return fail("native object ISA or linker atom contract does not match");
  auto Triple = sourcePatchTargetTriple(Original);
  if (!Triple)
    return Triple.takeError();
  if (Original.Arch == Arch::ARM) {
    const bool Watch = llvm::Triple(*Triple).isWatchOS();
    if (Watch
            ? Subtype != CPU_SUBTYPE_ARM_V7K
            : (Subtype != CPU_SUBTYPE_ARM_V7 && Subtype != CPU_SUBTYPE_ARM_V7S))
      return fail("native ARM platform ABI does not match the image");
  }
  NativeMachO Result;
  Result.SjLj =
      Original.Arch == Arch::ARM && !llvm::Triple(*Triple).isWatchOS();
  auto &Image = Result.Image;
  Image.InputKind = RewriteInputKind::ReplacementObject;
  Image.TargetArch = Original.Arch;
  Image.Format = BinaryFormat::MachO;
  Image.PointerWidth = Original.getPointerSize();
  Image.TargetTriple = *Triple;
  uint64_t Total = 0;
  for (const auto &S : M->sections()) {
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    const uint32_t Number = M->getSectionID(S) + 1;
    const uint32_t Flags = M->is64Bit()
                               ? M->getSection64(S.getRawDataRefImpl()).flags
                               : M->getSection(S.getRawDataRefImpl()).flags;
    const unsigned Type = Flags & SECTION_TYPE;
    const bool Compact = *Name == "__compact_unwind";
    if ((Flags & S_ATTR_DEBUG) && !Compact)
      continue;
    if (!S.getSize())
      continue;
    if (Name->starts_with("__thread") || *Name == "__mod_init_func" ||
        *Name == "__mod_term_func" || *Name == "__LLVM" ||
        M->getSectionFinalSegmentName(S.getRawDataRefImpl()) == "__LLVM")
      return fail("unsupported runtime initialization, TLS or bitcode section");
    const bool Code =
        Flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    const bool Zero = Type == S_ZEROFILL;
    if (Type != S_REGULAR && Type != S_COALESCED && Type != S_ZEROFILL &&
        Type != S_CSTRING_LITERALS && Type != S_4BYTE_LITERALS &&
        Type != S_8BYTE_LITERALS && Type != S_16BYTE_LITERALS)
      return fail("unsupported native section kind: " + *Name);
    if (S.getSize() > MaxStorage - Total)
      return fail("native storage exceeds profile limit");
    Total += S.getSize();
    CompiledSection Item;
    Item.Name = Name->str();
    Item.Size = S.getSize();
    Item.Alignment = S.getAlignment().value();
    const bool Writable = Zero || M->getSectionFinalSegmentName(
                                      S.getRawDataRefImpl()) == "__DATA";
    Item.Kind = Compact    ? Kind::Other
                : Code     ? Kind::Code
                : Zero     ? Kind::UninitializedData
                : Writable ? Kind::WritableData
                           : Kind::ReadOnlyData;
    Item.IsAllocated = !Compact;
    Item.IsInImage = !Compact && *Name != "__eh_frame";
    if (!llvm::isPowerOf2_64(Item.Alignment) || Item.Alignment > 65536)
      return fail("invalid section alignment");
    if (Zero)
      Item.ExternalBytes.resize(Item.Size, 0);
    else {
      auto Data = S.getContents();
      if (!Data)
        return Data.takeError();
      if (Data->size() != Item.Size)
        return fail("truncated section");
      Item.ExternalBytes.assign(Data->begin(), Data->end());
    }
    const size_t Index = Image.Sections.size();
    if (*Name == "__eh_frame") {
      if (Result.EH)
        return fail("duplicate native eh_frame");
      Result.EH = Index;
      auto Records = decodeDwarfEHFrameRecords(
          Item.ExternalBytes, S.getAddress(), Image.PointerWidth == 8);
      if (!Records)
        return Records.takeError();
      Result.OldFDEs = std::move(*Records);
    }
    if (Compact) {
      if (Result.CompactSection)
        return fail("duplicate compact input");
      Result.CompactSection = Index;
    }
    Result.Sections[Number] = Index;
    Result.OldSectionVAs[Number] = S.getAddress();
    Image.Sections.push_back(std::move(Item));
    auto Range = S.relocations();
    for (auto I = Range.begin(); I != Range.end(); ++I) {
      const auto Raw = M->getRelocation(I->getRawDataRefImpl());
      Relocation R{Index,
                   I->getOffset(),
                   M->getAnyRelocationType(Raw),
                   1u << M->getAnyRelocationLength(Raw),
                   M->getPlainRelocationSymbolNum(Raw),
                   M->getPlainRelocationExternal(Raw),
                   bool(M->getAnyRelocationPCRel(Raw)),
                   M->isRelocationScattered(Raw)};
      const bool Half =
          Original.Arch == Arch::ARM &&
          (R.Type == ARM_RELOC_HALF || R.Type == ARM_RELOC_HALF_SECTDIFF);
      if (Half) {
        R.HalfLength = M->getAnyRelocationLength(Raw);
        R.Width = 4;
      }
      if (R.Width != 4 && R.Width != 8)
        return fail("unsupported relocation width");
      if (Half) {
        const bool Difference = R.Type == ARM_RELOC_HALF_SECTDIFF;
        if (R.PCRel || R.Scattered != Difference || ++I == Range.end())
          return fail("malformed ARM HALF pair");
        const auto Pair = M->getRelocation(I->getRawDataRefImpl());
        if (M->isRelocationScattered(Pair) != Difference ||
            M->getAnyRelocationPCRel(Pair) ||
            M->getAnyRelocationType(Pair) != ARM_RELOC_PAIR ||
            M->getAnyRelocationLength(Pair) != R.HalfLength ||
            (!Difference && M->getPlainRelocationExternal(Pair)))
          return fail("invalid ARM HALF pair");
        R.OtherHalf = M->getAnyRelocationAddress(Pair) & 0xffff;
        if (Difference) {
          R.ScatteredVA = M->getScatteredRelocationValue(Raw);
          R.SubtractVA = M->getScatteredRelocationValue(Pair);
        }
      } else if (R.Scattered) {
        R.ScatteredVA = M->getScatteredRelocationValue(Raw);
        if (R.Type == GENERIC_RELOC_SECTDIFF ||
            R.Type == GENERIC_RELOC_LOCAL_SECTDIFF ||
            (Original.Arch == Arch::ARM &&
             (R.Type == ARM_RELOC_SECTDIFF ||
              R.Type == ARM_RELOC_LOCAL_SECTDIFF))) {
          if (++I == Range.end())
            return fail("section difference has no pair");
          const auto Pair = M->getRelocation(I->getRawDataRefImpl());
          if (!M->isRelocationScattered(Pair) ||
              M->getAnyRelocationType(Pair) != GENERIC_RELOC_PAIR ||
              (1u << M->getAnyRelocationLength(Pair)) != R.Width || R.PCRel)
            return fail("invalid section difference pair");
          R.SubtractVA = M->getScatteredRelocationValue(Pair);
        } else if (R.Type != GENERIC_RELOC_VANILLA)
          return fail("unsupported scattered native relocation");
      } else if ((Original.Arch == Arch::X64 &&
                  R.Type == X86_64_RELOC_SUBTRACTOR) ||
                 (Original.Arch == Arch::AArch64 &&
                  R.Type == ARM64_RELOC_SUBTRACTOR)) {
        if (!R.External || ++I == Range.end())
          return fail("subtractor has no symbol pair");
        const auto Pair = M->getRelocation(I->getRawDataRefImpl());
        const unsigned UnsignedType = Original.Arch == Arch::X64
                                          ? unsigned(X86_64_RELOC_UNSIGNED)
                                          : unsigned(ARM64_RELOC_UNSIGNED);
        if (M->isRelocationScattered(Pair) ||
            !M->getPlainRelocationExternal(Pair) ||
            M->getAnyRelocationType(Pair) != UnsignedType ||
            I->getOffset() != R.Offset ||
            (1u << M->getAnyRelocationLength(Pair)) != R.Width || R.PCRel)
          return fail("invalid subtractor pair");
        R.SubtractSymbol = R.SymbolIndex;
        R.SymbolIndex = M->getPlainRelocationSymbolNum(Pair);
        R.Type = UnsignedType;
      }
      Result.Relocations.push_back(R);
    }
  }
  for (const auto &S : M->symbols()) {
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    const auto Index = M->getSymbolIndex(S.getRawDataRefImpl());
    const uint8_t Type =
        M->is64Bit() ? M->getSymbol64TableEntry(S.getRawDataRefImpl()).n_type
                     : M->getSymbolTableEntry(S.getRawDataRefImpl()).n_type;
    const uint16_t Desc =
        M->is64Bit() ? M->getSymbol64TableEntry(S.getRawDataRefImpl()).n_desc
                     : M->getSymbolTableEntry(S.getRawDataRefImpl()).n_desc;
    const uint32_t Number = M->getSymbolSectionID(S) + 1;
    const uint64_t Value =
        M->is64Bit() ? M->getSymbol64TableEntry(S.getRawDataRefImpl()).n_value
                     : M->getSymbolTableEntry(S.getRawDataRefImpl()).n_value;
    if (Type & N_STAB)
      continue;
    if ((Type & N_TYPE) != N_SECT && (Type & N_TYPE) != N_UNDF)
      continue;
    if ((Desc & (N_WEAK_REF | N_WEAK_DEF)) ||
        ((Type & N_TYPE) == N_UNDF && Value))
      return fail("weak/common native definitions are unsupported");
    Symbol Item{
        canonical(*Name), Number, 0, 0, false, bool(Desc & N_ARM_THUMB_DEF)};
    if ((Type & N_TYPE) == N_SECT) {
      if (!Result.Sections.contains(Number))
        continue;
      const auto &Section = Image.Sections[Result.Sections.at(Number)];
      if (Value < Result.OldSectionVAs.at(Number) ||
          Value - Result.OldSectionVAs.at(Number) >= Section.Size)
        return fail("symbol exceeds native section");
      Item.Offset = Value - Result.OldSectionVAs.at(Number);
      Item.Function = Section.Kind == Kind::Code && Name->starts_with("_");
    } else
      Item.Section = 0;
    if (!Name->empty() && !Result.Names.emplace(Item.Name, Index).second)
      return fail("duplicate native symbol");
    Result.Symbols.emplace(Index, Item);
  }
  return Result;
}

llvm::Error place(NativeMachO &Object, uint64_t Base,
                  const std::optional<MachOEHFrameRegion> &EH) {
  auto &Image = Object.Image;
  Image.BaseVA = Base;
  uint64_t Cursor = Base;
  for (Kind Group : {Kind::Code, Kind::ReadOnlyData, Kind::WritableData,
                     Kind::UninitializedData}) {
    if (Group == Kind::WritableData)
      Cursor = llvm::alignTo(
          Cursor, sourcePatchStorageAlignment(Image.TargetArch, Image.Format));
    for (auto &S : Image.Sections)
      if (S.IsAllocated && S.IsInImage && S.Kind == Group) {
        Cursor = llvm::alignTo(Cursor, S.Alignment);
        if (Cursor < Base || Cursor - Base > MaxStorage ||
            S.Size > MaxStorage - (Cursor - Base))
          return fail("native placement overflows");
        S.VA = Cursor;
        S.Offset = Cursor - Base;
        Cursor += S.Size;
      }
  }
  if (Cursor - Base > MaxStorage)
    return fail("native placement exceeds limit");
  Image.Bytes.resize(Cursor - Base, 0);
  for (auto &S : Image.Sections)
    if (S.IsInImage) {
      std::copy(S.ExternalBytes.begin(), S.ExternalBytes.end(),
                Image.Bytes.begin() + S.Offset);
      S.ExternalBytes.clear();
    }
  if (Object.EH) {
    if (!EH)
      return fail(
          "native DWARF requires a registered original eh_frame section");
    Image.Sections[*Object.EH].VA = EH->AppendVA;
  }
  for (const auto &[Index, S] : Object.Symbols)
    if (S.Section && Object.Sections.contains(S.Section))
      Image.SymbolAddrs[S.Name] =
          Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset;
  Image.Success = true;
  return llvm::Error::success();
}
llvm::Expected<uint64_t> mapOldVA(const NativeMachO &Object, uint64_t VA) {
  std::optional<uint64_t> Value;
  for (const auto &[Number, Index] : Object.Sections) {
    const auto &S = Object.Image.Sections[Index];
    const uint64_t Old = Object.OldSectionVAs.at(Number);
    if (!S.IsAllocated || VA < Old || VA - Old >= S.Size)
      continue;
    if (Value)
      return fail("ambiguous object address");
    Value = S.VA + VA - Old;
  }
  if (!Value)
    return fail("object address does not name allocated storage");
  return *Value;
}
llvm::Expected<uint64_t>
resolve(const NativeMachO &Object, unsigned Index, const BinaryImage &Original,
        const std::map<std::string, SourcePatchBinding> &Bindings) {
  if (!Object.Symbols.contains(Index))
    return fail("relocation symbol is invalid");
  const auto &S = Object.Symbols.at(Index);
  if (S.Section) {
    if (!Object.Sections.contains(S.Section))
      return fail("nonallocated native symbol");
    return Object.Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset +
           (S.Function && S.Thumb ? 1 : 0);
  }
  if (auto B = Bindings.find(S.Name); B != Bindings.end())
    return B->second.OriginalVA;
  std::set<uint64_t> VAs;
  for (const auto &S0 : Original.Symbols)
    if (canonical(S0.Name) == S.Name && S0.IsFunc &&
        Original.hasAuthenticatedFunctionEntryAt(S0.Addr))
      VAs.insert(S0.Addr);
  for (const auto &[VA, Import] : Original.ImportStubIndices)
    if (Import < Original.Imports.size() &&
        Original.Imports[Import].Name == S.Name &&
        Original.findImportStubAt(VA))
      VAs.insert(VA);
  if (VAs.size() != 1)
    return fail(
        "external native symbol needs a binding or authenticated callable: " +
        S.Name);
  return *VAs.begin();
}
llvm::Error
relocate(NativeMachO &Object, const BinaryImage &Original,
         const std::map<std::string, SourcePatchBinding> &Bindings) {
  const Arch A = Original.Arch;
  std::map<std::pair<size_t, uint64_t>, unsigned> Written;
  for (const auto &R : Object.Relocations) {
    const auto &S = Object.Image.Sections[R.Section];
    if (!rangeInBounds(R.Offset, R.Width, S.Size))
      return fail("relocation exceeds native section");
    auto Key = std::pair{R.Section, R.Offset};
    auto Next = Written.lower_bound(Key);
    if ((Next != Written.end() && Next->first.first == R.Section &&
         Next->first.second - R.Offset < R.Width) ||
        (Next != Written.begin() && std::prev(Next)->first.first == R.Section &&
         R.Offset - std::prev(Next)->first.second < std::prev(Next)->second))
      return fail("overlapping native relocation writers");
    Written.emplace(Key, R.Width);
    uint8_t *Out =
        S.IsInImage
            ? Object.Image.Bytes.data() + S.Offset + R.Offset
            : Object.Image.Sections[R.Section].ExternalBytes.data() + R.Offset;
    const uint64_t Raw = R.Width == 8 ? readLE<uint64_t>(Out)
                                      : readLE<uint32_t>(Out),
                   P = S.VA + R.Offset;
    const auto Number = llvm::find_if(Object.Sections, [&](const auto &X) {
                          return X.second == R.Section;
                        })->first;
    const uint64_t OldP = Object.OldSectionVAs.at(Number) + R.Offset;
    llvm::Expected<uint64_t> Target =
        R.Scattered  ? mapOldVA(Object, R.ScatteredVA)
        : R.External ? resolve(Object, R.SymbolIndex, Original, Bindings)
        : Object.Sections.contains(R.SymbolIndex)
            ? llvm::Expected<uint64_t>(
                  Object.Image.Sections[Object.Sections.at(R.SymbolIndex)].VA)
            : llvm::Expected<uint64_t>(
                  fail("invalid relocation section ordinal"));
    if (!Target)
      return Target.takeError();
    llvm::APInt V(128, *Target);
    const bool Vanilla =
        (A == Arch::X86 && R.Type == GENERIC_RELOC_VANILLA) ||
        (A == Arch::ARM && R.Type == ARM_RELOC_VANILLA) ||
        (A == Arch::X64 &&
         (R.Type == X86_64_RELOC_UNSIGNED || R.Type == X86_64_RELOC_BRANCH ||
          R.Type == X86_64_RELOC_SIGNED || R.Type == X86_64_RELOC_SIGNED_1 ||
          R.Type == X86_64_RELOC_SIGNED_2 ||
          R.Type == X86_64_RELOC_SIGNED_4)) ||
        (A == Arch::AArch64 && R.Type == ARM64_RELOC_UNSIGNED);
    const bool Half = A == Arch::ARM && (R.Type == ARM_RELOC_HALF ||
                                         R.Type == ARM_RELOC_HALF_SECTDIFF);
    if (Half) {
      const bool High = R.HalfLength & 1, Thumb = R.HalfLength & 2;
      uint16_t EncodedHalf;
      if (Thumb) {
        const uint16_t H = Raw, L = Raw >> 16;
        if ((H & 0xfbf0) != (High ? 0xf2c0 : 0xf240) || (L & 0x8000))
          return fail("ARM HALF does not name the selected Thumb MOV");
        EncodedHalf = ((H & 0xf) << 12) | ((H & 0x400) << 1) |
                      ((L & 0x7000) >> 4) | (L & 0xff);
      } else {
        if ((uint32_t(Raw) & 0x0ff00000) != (High ? 0x03400000 : 0x03000000))
          return fail("ARM HALF does not name the selected ARM MOV");
        EncodedHalf = ((Raw >> 4) & 0xf000) | (Raw & 0xfff);
      }
      const uint32_t Encoded =
          High ? (uint32_t(EncodedHalf) << 16) | R.OtherHalf
               : (uint32_t(R.OtherHalf) << 16) | EncodedHalf;
      if (!R.SubtractVA)
        return fail(
            "absolute ARM HALF instructions need dyld address registration");
      auto Sub = mapOldVA(Object, *R.SubtractVA);
      if (!Sub)
        return Sub.takeError();
      V -= llvm::APInt(128, *Sub);
      const uint32_t Addend = Encoded - uint32_t(R.ScatteredVA - *R.SubtractVA);
      V += llvm::APInt(32, Addend).sext(128);
      if (!V.isSignedIntN(32) && !V.isIntN(32))
        return fail("ARM HALF difference overflows");
      const uint32_t Value = V.getLoBits(32).getZExtValue();
      const uint16_t Part = High ? Value >> 16 : Value;
      if (Thumb) {
        const uint16_t H = Raw, L = Raw >> 16;
        writeLE<uint16_t>(Out, (H & ~0x040f) | ((Part >> 12) & 0xf) |
                                   ((Part >> 1) & 0x0400));
        writeLE<uint16_t>(Out + 2, (L & ~0x70ff) | ((Part << 4) & 0x7000) |
                                       (Part & 0xff));
      } else
        writeLE<uint32_t>(Out, (uint32_t(Raw) & ~0x000f0fff) |
                                   ((uint32_t(Part) & 0xf000) << 4) |
                                   (Part & 0xfff));
    } else if (R.SubtractVA || R.SubtractSymbol) {
      auto Sub = R.SubtractVA
                     ? mapOldVA(Object, *R.SubtractVA)
                     : resolve(Object, *R.SubtractSymbol, Original, Bindings);
      if (!Sub)
        return Sub.takeError();
      V -= llvm::APInt(128, *Sub);
      V += llvm::APInt(R.Width * 8, Raw).sext(128);
      if (R.SubtractVA) {
        V -= llvm::APInt(128, R.ScatteredVA);
        V += llvm::APInt(128, *R.SubtractVA);
      }
      if (!V.isSignedIntN(R.Width * 8))
        return fail("native section difference overflows");
      if (R.Width == 8)
        writeLE<uint64_t>(Out, V.getLoBits(64).getZExtValue());
      else
        writeLE<uint32_t>(Out, V.getLoBits(32).getZExtValue());
    } else if (Vanilla) {
      if (R.PCRel) {
        V += llvm::APInt(128, uint64_t(int64_t(int32_t(Raw))), true);
        unsigned Bias = R.Type == X86_64_RELOC_SIGNED_1   ? 1
                        : R.Type == X86_64_RELOC_SIGNED_2 ? 2
                        : R.Type == X86_64_RELOC_SIGNED_4 ? 4
                                                          : 0;
        if (!R.External || A == Arch::X86) {
          V += llvm::APInt(128, OldP + R.Width);
          if (!R.External)
            V -= llvm::APInt(128, R.Scattered
                                      ? R.ScatteredVA
                                      : Object.OldSectionVAs.at(R.SymbolIndex));
        }
        V -= llvm::APInt(128, P + R.Width + Bias);
        if (!V.isSignedIntN(32))
          return fail("native PC-relative relocation overflows");
        writeLE<uint32_t>(Out, V.getLoBits(32).getZExtValue());
      } else {
        V += llvm::APInt(128, Raw);
        if (!R.External)
          V -= llvm::APInt(128, R.Scattered
                                    ? R.ScatteredVA
                                    : Object.OldSectionVAs.at(R.SymbolIndex));
        if (!V.isIntN(R.Width * 8))
          return fail("native pointer relocation overflows");
        if (R.Width == 8)
          writeLE<uint64_t>(Out, V.getLoBits(64).getZExtValue());
        else
          writeLE<uint32_t>(Out, V.getLoBits(32).getZExtValue());
        if (S.IsAllocated && S.IsInImage)
          return fail(
              "native absolute pointer needs dyld relocation registration");
      }
    } else if (A == Arch::AArch64 &&
               (R.Type == ARM64_RELOC_PAGE21 ||
                R.Type == ARM64_RELOC_PAGEOFF12) &&
               R.Width == 4) {
      const uint32_t Old = Raw;
      if (R.Type == ARM64_RELOC_PAGE21) {
        if (!R.PCRel || (Old & 0x9f000000) != 0x90000000 || !R.External)
          return fail("PAGE21 requires a symbolic ADRP");
        V += llvm::APInt(
            128,
            uint64_t(llvm::SignExtend64<21>((((Old >> 5) & 0x7ffff) << 2) |
                                            ((Old >> 29) & 3)) *
                     4096),
            true);
        V &= ~llvm::APInt(128, 4095);
        V -= llvm::APInt(128, P & ~uint64_t(4095));
        if (!V.isSignedIntN(33))
          return fail("native PAGE21 overflows");
        const uint32_t I = V.getLoBits(33).getZExtValue() >> 12;
        writeLE<uint32_t>(Out, (Old & 0x9f00001f) | ((I & 3) << 29) |
                                   (((I >> 2) & 0x7ffff) << 5));
      } else {
        if (R.PCRel || !R.External)
          return fail("PAGEOFF12 requires a symbolic unshifted address");
        unsigned Scale = 0;
        if ((Old & 0x3b000000) == 0x39000000) {
          Scale = Old >> 30;
          if (!Scale && (Old & 0x04800000) == 0x04800000)
            Scale = 4;
        } else if ((Old & 0x7fc00000) != 0x11000000)
          return fail("PAGEOFF12 does not name an unsigned load/store or "
                      "unshifted ADD");
        V += llvm::APInt(128, uint64_t((Old >> 10) & 0xfff) << Scale);
        const uint32_t I = V.getLoBits(12).getZExtValue();
        if (I & ((1u << Scale) - 1))
          return fail("unaligned PAGEOFF12 access");
        writeLE<uint32_t>(Out, (Old & ~0x003ffc00) | ((I >> Scale) << 10));
      }
    } else if (A == Arch::ARM && R.Type == ARM_THUMB_RELOC_BR22 && R.PCRel &&
               R.Width == 4) {
      const uint16_t H = readLE<uint16_t>(Out), L = readLE<uint16_t>(Out + 2);
      if ((H & 0xf800) != 0xf000 || (L & 0xf800) != 0xf800)
        return fail("BR22 does not name the Darwin Thumb branch pair");
      const int64_t Addend =
          llvm::SignExtend64<23>(((H & 0x7ff) << 12) | ((L & 0x7ff) << 1));
      V += llvm::APInt(128, uint64_t(Addend), true);
      if (!R.External) {
        V += llvm::APInt(128, OldP + 4);
        V -= llvm::APInt(128, Object.OldSectionVAs.at(R.SymbolIndex));
      } else
        V += llvm::APInt(128, OldP + 4);
      V.clearBit(0);
      const uint64_t TargetVA = V.getLoBits(64).getZExtValue();
      const bool Thumb =
          llvm::any_of(Object.Symbols,
                       [&](const auto &S0) {
                         const auto &S = S0.second;
                         return S.Function && S.Thumb &&
                                Object.Image.SymbolAddrs.at(S.Name) == TargetVA;
                       }) ||
          (Original.instructionModeAt(TargetVA) == InstructionMode::Thumb);
      if (!Thumb)
        return fail("BR22 requires a verified Thumb target");
      V -= llvm::APInt(128, P + 4);
      if (!V.isSignedIntN(23) || V.getLoBits(1).getZExtValue())
        return fail("BR22 displacement overflows");
      const uint32_t D = V.getLoBits(23).getZExtValue();
      writeLE<uint16_t>(Out, (H & 0xf800) | ((D >> 12) & 0x7ff));
      writeLE<uint16_t>(Out + 2, (L & 0xf800) | ((D >> 1) & 0x7ff));
    } else if (A == Arch::AArch64 && R.Type == ARM64_RELOC_BRANCH26 &&
               R.PCRel && R.Width == 4) {
      const uint32_t Old = Raw;
      if ((Old & 0x7c000000) != 0x14000000)
        return fail("BRANCH26 does not name B/BL");
      V += llvm::APInt(
          128, uint64_t(llvm::SignExtend64<26>(Old & 0x3ffffff) * 4), true);
      V -= llvm::APInt(128, P);
      if (!V.isSignedIntN(28) || (V.getLoBits(2).getZExtValue()))
        return fail("native branch overflows");
      writeLE<uint32_t>(
          Out, (Old & 0xfc000000) |
                   ((V.getLoBits(28).getZExtValue() >> 2) & 0x3ffffff));
    } else
      return fail("unsupported native relocation " + llvm::Twine(R.Type));
  }
  if (Object.EH) {
    auto &EH = Object.Image.Sections[*Object.EH];
    for (const auto &R : Object.OldFDEs) {
      if (R.HasLanguageAugmentation ||
          ((R.InitialLocationEncoding & 0x70) != 0x10) ||
          (R.InitialLocationEncoding & 0x80) ||
          (R.InitialLocationWidth != 4 && R.InitialLocationWidth != 8))
        return fail("native DWARF language or address encoding requires a "
                    "separate contract");
      auto New = mapOldVA(Object, R.BeginVA);
      if (!New)
        return New.takeError();
      llvm::APInt D(128, *New);
      D -= llvm::APInt(128, EH.VA + R.InitialLocationOffset);
      if (!D.isSignedIntN(R.InitialLocationWidth * 8))
        return fail("native FDE initial location overflows");
      auto *Field = EH.ExternalBytes.data() + R.InitialLocationOffset;
      if (R.InitialLocationWidth == 8)
        writeLE<uint64_t>(Field, D.getLoBits(64).getZExtValue());
      else
        writeLE<uint32_t>(Field, D.getLoBits(32).getZExtValue());
    }
  }
  std::map<uint64_t, uint64_t> Extents;
  if (Object.CompactSection) {
    const auto &Data =
        Object.Image.Sections[*Object.CompactSection].ExternalBytes;
    const unsigned Ptr = Object.Image.PointerWidth, Stride = Ptr * 3 + 8;
    if (Data.size() % Stride)
      return fail("truncated native compact records");
    for (size_t O = 0; O < Data.size(); O += Stride) {
      const uint64_t Begin = Ptr == 8 ? readLE<uint64_t>(Data.data() + O)
                                      : readLE<uint32_t>(Data.data() + O);
      const uint32_t Length = readLE<uint32_t>(Data.data() + O + Ptr),
                     Encoding = readLE<uint32_t>(Data.data() + O + Ptr + 4);
      const uint64_t Personality =
          Ptr == 8 ? readLE<uint64_t>(Data.data() + O + Ptr + 8)
                   : readLE<uint32_t>(Data.data() + O + Ptr + 8);
      const uint64_t LSDA =
          Ptr == 8 ? readLE<uint64_t>(Data.data() + O + 2 * Ptr + 8)
                   : readLE<uint32_t>(Data.data() + O + 2 * Ptr + 8);
      if (!Length || Personality || LSDA ||
          !Extents.emplace(Begin, Begin + Length).second)
        return fail("native compact language/range contract is unsupported");
      NativeMachOCompactRange Range{Begin, Begin + Length, Encoding};
      const uint32_t Mode = Encoding & macho_unwind::kModeMask;
      const bool Dwarf =
          (A == Arch::AArch64 ? Mode == macho_unwind::kARM64ModeDwarf
                              : Mode == macho_unwind::kX86ModeDwarf);
      if (!Dwarf)
        Object.Compact.push_back(Range);
    }
  }
  if (Object.EH) {
    const auto &EH = Object.Image.Sections[*Object.EH];
    auto Records = decodeDwarfEHFrameRecords(EH.ExternalBytes, EH.VA,
                                             Object.Image.PointerWidth == 8);
    if (!Records)
      return Records.takeError();
    for (const auto &R : *Records) {
      auto [It, Added] = Extents.emplace(R.BeginVA, R.EndVA);
      if (!Added && It->second != R.EndVA)
        return fail("native DWARF and compact extents disagree");
    }
  }
  std::map<uint64_t, Symbol *> Functions;
  for (auto &[I, S] : Object.Symbols)
    if (S.Function) {
      const auto &Section =
          Object.Image.Sections[Object.Sections.at(S.Section)];
      const uint64_t Begin = Section.VA + S.Offset;
      if (!Functions.emplace(Begin, &S).second)
        return fail("aliased native function entries are ambiguous");
    }
  for (auto I = Functions.begin(); I != Functions.end(); ++I) {
    auto &S = *I->second;
    const auto &Section = Object.Image.Sections[Object.Sections.at(S.Section)];
    if (Extents.contains(I->first))
      S.Size = Extents.at(I->first) - I->first;
    else if (Object.SjLj) {
      auto Next = std::next(I);
      S.Size = (Next != Functions.end() && Next->second->Section == S.Section
                    ? Next->first
                    : Section.VA + Section.Size) -
               I->first;
    } else
      return fail("native function lacks registered unwind coverage");
    if (!S.Size || !rangeInBounds(S.Offset, S.Size, Section.Size))
      return fail("native function extent exceeds code section");
  }
  for (const auto &[Begin, End] : Extents)
    if (!Functions.contains(Begin) || Functions.at(Begin)->Size != End - Begin)
      return fail("native unwind has no exact function symbol");
  auto OriginalCompact = findMachOCompactUnwindRegion(Original.Raw);
  if (!OriginalCompact)
    return OriginalCompact.takeError();
  if (!*OriginalCompact && !Object.Compact.empty() && Object.EH) {
    const auto &EH = Object.Image.Sections[*Object.EH];
    auto Records = decodeDwarfEHFrameRecords(EH.ExternalBytes, EH.VA,
                                             Object.Image.PointerWidth == 8);
    if (!Records)
      return Records.takeError();
    if (llvm::all_of(Object.Compact, [&](const auto &Range) {
          return llvm::count_if(*Records, [&](const auto &R) {
                   return R.BeginVA == Range.BeginVA && R.EndVA == Range.EndVA;
                 }) == 1;
        }))
      Object.Compact.clear();
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<PatchResult> installMachOObjectPatch(
    llvm::StringRef Bytes, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> RequestedBindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt) {
  auto Object = readObject(Bytes, Original);
  if (!Object)
    return Object.takeError();
  std::set<std::string> Entries;
  std::set<uint64_t> Addresses;
  for (const auto &M : Mappings) {
    if (!Object->Names.contains(M.Symbol) || !Entries.insert(M.Symbol).second ||
        !Addresses.insert(M.OriginalVA).second ||
        !Original.hasAuthenticatedFunctionEntryAt(M.OriginalVA) ||
        !sourceEntryCanHostTrampoline(Original, M.OriginalVA))
      return fail("invalid native entry mapping");
    const auto &S = Object->Symbols.at(Object->Names.at(M.Symbol));
    if (!S.Function || !S.Section)
      return fail("entry needs a native function definition");
  }
  BinaryImage References = Original;
  scanImportThunks(References);
  std::map<std::string, SourcePatchBinding> Bindings;
  for (const auto &B : RequestedBindings) {
    if (!Object->Names.contains(B.Symbol) ||
        Object->Symbols.at(Object->Names.at(B.Symbol)).Section ||
        !Bindings.emplace(B.Symbol, B).second)
      return fail("binding needs a unique undefined native symbol");
    if (auto E = validateSourcePatchBinding(B, References, Mappings))
      return std::move(E);
  }
  MachOPatcher Patcher;
  Patcher.setInputKind(RewriteInputKind::ReplacementObject);
  const uint64_t Base =
      Patcher.plannedExecSegmentVA(Original.Raw, Original.Arch);
  const auto EH = findMachOEHFrameRegion(Original.Raw);
  if (!Base)
    return fail("original Mach-O has no safe placement");
  if (auto E = place(*Object, Base, EH))
    return std::move(E);
  if (auto E = relocate(*Object, References, Bindings))
    return std::move(E);
  return publishSourcePatch(
      Original, Mappings, Output,
      [&](const std::filesystem::path &Input,
          const std::filesystem::path &Candidate)
          -> llvm::Expected<PatchResult> {
        std::string Failure;
        auto R = BinaryPatcher::readPatchWrite(
            Input, Candidate, true, "object_patch",
            [&](std::vector<uint8_t> &Binary, PatchResult &Result) {
              auto Reject = [&](llvm::Error E) {
                Failure = llvm::toString(std::move(E));
                return false;
              };
              if (Object->EH)
                if (auto E = installNativeMachOEHFrame(
                        Binary, *EH, Object->Image.Sections[*Object->EH]))
                  return Reject(std::move(E));
              if (auto E = installNativeMachOCompactUnwind(
                      Binary, Original.Arch, Object->Compact))
                return Reject(std::move(E));
              if (auto E =
                      Patcher.appendReplacementImage(Binary, Object->Image))
                return Reject(std::move(E));
              for (const auto &M : Mappings) {
                const auto &S = Object->Symbols.at(Object->Names.at(M.Symbol));
                const auto Mode = Original.instructionModeAt(M.OriginalVA);
                const uint64_t Owner = Object->Image.SymbolAddrs.at(M.Symbol);
                if (!Mode ||
                    (Original.Arch == Arch::ARM &&
                     (S.Thumb ? InstructionMode::Thumb
                              : InstructionMode::ARM) != *Mode) ||
                    !getTargetCodegenInfo(Original.Arch, *Mode)
                         .writeSourceTrampoline(
                             Binary, Original.vaToFileOffset(M.OriginalVA),
                             Owner, M.OriginalVA,
                             getTargetCodegenInfo(Original.Arch, *Mode)
                                 .sourceTrampolineSize()))
                  return Reject(
                      fail("native entry mode or branch is incompatible"));
                Result.PatchedFunctions.push_back(
                    {M.Symbol, M.OriginalVA, Owner, M.Symbol});
                Result.PatchedOriginalEntries.push_back(M.OriginalVA);
              }
              llvm::sort(Result.PatchedOriginalEntries);
              Result.CodeSize = Object->Image.Bytes.size();
              Result.TrampolineCount = Mappings.size();
              return true;
            });
        if (!R.Success)
          return fail(Failure.empty() ? "candidate write failed" : Failure);
        return R;
      },
      PrepareReceipt);
}
} // namespace neverd
