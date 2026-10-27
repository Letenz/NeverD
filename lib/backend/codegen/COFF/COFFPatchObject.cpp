//===- COFFPatchObject.cpp - Native COFF replacement installation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFSourceImage.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/backend/codegen/SourcePatchPreparation.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/object/PELayout.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {
using Kind = llvm::mc_rewrite::RewriteSectionKind;
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "COFF patch object: " + Message);
}
std::string canonicalName(llvm::StringRef Name, Arch A) {
  return (A == Arch::X86 && Name.starts_with("_") ? Name.drop_front() : Name)
      .str();
}
struct Symbol {
  std::string Name;
  int32_t Section = 0;
  uint64_t Offset = 0, Size = 0;
  bool Function = false, Thumb = false;
};
struct Relocation {
  size_t Section;
  uint64_t Offset;
  uint32_t SymbolIndex;
  unsigned Type;
};
struct NativeCOFF {
  CompiledImage Image;
  COFFObjectEvidence Evidence;
  std::map<int32_t, size_t> Sections;
  std::map<uint32_t, Symbol> Symbols;
  std::map<std::string, uint32_t> Names;
  std::vector<Relocation> Relocations;
};

llvm::Expected<NativeCOFF> readObject(llvm::StringRef Bytes,
                                      const BinaryImage &Original) {
  auto File = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Bytes, "patch-object"));
  if (!File)
    return File.takeError();
  const uint16_t Machine =
      Original.Arch == Arch::X86       ? llvm::COFF::IMAGE_FILE_MACHINE_I386
      : Original.Arch == Arch::X64     ? llvm::COFF::IMAGE_FILE_MACHINE_AMD64
      : Original.Arch == Arch::ARM     ? llvm::COFF::IMAGE_FILE_MACHINE_ARMNT
      : Original.Arch == Arch::AArch64 ? llvm::COFF::IMAGE_FILE_MACHINE_ARM64
                                       : 0;
  if ((*File)->getMachine() != Machine || (*File)->getDOSHeader() ||
      ((*File)->getCOFFHeader() &&
       (*File)->getCOFFHeader()->SizeOfOptionalHeader) ||
      (*File)->getNumberOfSymbols() >
          ::neverd::limits::backend::kMaxCOFFPatchObjectSymbols)
    return fail("expected a matching native relocatable COFF object");
  NativeCOFF Result;
  auto &Image = Result.Image;
  Image.InputKind = RewriteInputKind::ReplacementObject;
  Image.TargetArch = Original.Arch;
  Image.Format = BinaryFormat::COFF;
  Image.PointerWidth = Original.getPointerSize();
  auto Target = sourcePatchTargetTriple(Original);
  if (!Target)
    return Target.takeError();
  Image.TargetTriple = *Target;
  uint64_t Total = 0;
  std::map<int32_t, uint64_t> SectionLengths;
  for (const auto &Section : (*File)->sections()) {
    auto Name = Section.getName();
    if (!Name)
      return Name.takeError();
    const auto *Raw = (*File)->getCOFFSection(Section);
    const int32_t Number = Section.getIndex() + 1;
    const uint32_t Flags = Raw->Characteristics;
    const uint64_t Size = Section.getSize();
    if (Name->starts_with(".tls") || Name->starts_with(".CRT") ||
        Name->starts_with(".sxdata") || Name->starts_with(".gfids") ||
        Name->starts_with(".gehcont") || Name->starts_with(".llvm.lto") ||
        Name->starts_with(".llvmbc") || (*Name == ".drectve" && Size))
      return fail("unsupported runtime or linker contract: " + *Name);
    if (!Size ||
        (Flags & (llvm::COFF::IMAGE_SCN_LNK_REMOVE |
                  llvm::COFF::IMAGE_SCN_MEM_DISCARDABLE)) ||
        Name->starts_with(".debug") || *Name == ".llvm_addrsig")
      continue;
    const bool Code = Flags & llvm::COFF::IMAGE_SCN_CNT_CODE;
    const bool Writable = Flags & llvm::COFF::IMAGE_SCN_MEM_WRITE;
    const bool Zero = Flags & llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    if (!(Flags & llvm::COFF::IMAGE_SCN_MEM_READ) || (Code && Writable) ||
        bool(Flags & llvm::COFF::IMAGE_SCN_MEM_EXECUTE) != Code ||
        (Zero && !Writable) ||
        (!Code && !(Flags & (llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                             llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA))) ||
        Size > ::neverd::limits::backend::kCOFFPatchObjectMaxStorage - Total)
      return fail("invalid allocated section permissions or size: " + *Name);
    Total += Size;
    CompiledSection Item;
    Item.Name = Name->str();
    Item.Size = Size;
    Item.Alignment = Section.getAlignment().value();
    Item.Kind = Code       ? Kind::Code
                : Zero     ? Kind::UninitializedData
                : Writable ? Kind::WritableData
                           : Kind::ReadOnlyData;
    if (!llvm::isPowerOf2_64(Item.Alignment) || Item.Alignment > 65536)
      return fail("unsupported section alignment");
    if (Zero)
      Item.ExternalBytes.resize(Size, 0);
    else {
      auto Data = Section.getContents();
      if (!Data)
        return Data.takeError();
      if (Data->size() != Size)
        return fail("section contents are truncated");
      Item.ExternalBytes.assign(Data->begin(), Data->end());
    }
    Result.Sections.emplace(Number, Image.Sections.size());
    Image.Sections.push_back(std::move(Item));
    for (const auto &R : Section.relocations()) {
      const auto *RawReloc = (*File)->getCOFFRelocation(R);
      Result.Relocations.push_back({Result.Sections.at(Number), R.getOffset(),
                                    RawReloc->SymbolTableIndex,
                                    unsigned(R.getType())});
    }
  }
  std::map<int32_t, std::vector<uint32_t>> Functions;
  for (const auto &S : (*File)->symbols()) {
    const auto Raw = (*File)->getCOFFSymbol(S);
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    const uint32_t Index = (*File)->getSymbolIndex(Raw);
    if (Raw.isCommon() || Raw.isWeakExternal())
      return fail("unsupported common/weak symbol: " + *Name);
    if (auto Aux = Raw.getSectionDefinition();
        Aux && Raw.isSectionDefinition() &&
        Raw.getComplexType() != llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION) {
      SectionLengths[Raw.getSectionNumber()] = Aux->Length;
      if (Aux->Selection == llvm::COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE &&
          (Aux->getNumber(((*File)->getCOFFBigObjHeader() != nullptr)) <= 0 ||
           Aux->getNumber(((*File)->getCOFFBigObjHeader() != nullptr)) ==
               Raw.getSectionNumber()))
        return fail("invalid associative COMDAT owner");
    }
    Symbol Item{canonicalName(*Name, Original.Arch),
                Raw.getSectionNumber(),
                Raw.getValue(),
                0,
                Raw.getComplexType() == llvm::COFF::IMAGE_SYM_DTYPE_FUNCTION,
                false};
    if (Item.Section > 0 && Result.Sections.contains(Item.Section)) {
      const auto &Section = Image.Sections[Result.Sections.at(Item.Section)];
      if (!rangeInBounds(Item.Offset, 0, Section.Size))
        return fail("symbol exceeds native section");
      if (Item.Function) {
        if (Section.Kind != Kind::Code || Item.Offset >= Section.Size)
          return fail("function does not name code");
        Item.Thumb = Original.Arch == Arch::ARM;
        if (Raw.getNumberOfAuxSymbols() && Raw.isFunctionDefinition())
          Item.Size = Raw.getAux<llvm::object::coff_aux_function_definition>()
                          ->TotalSize;
        Functions[Item.Section].push_back(Index);
      }
    }
    if (!Name->empty() && !Raw.isSection() && !Raw.isSectionDefinition() &&
        !Raw.isFileRecord() && !Raw.isAbsolute()) {
      if (!Result.Names.emplace(Item.Name, Index).second)
        return fail("duplicate symbol: " + Item.Name);
    }
    Result.Symbols.emplace(Index, std::move(Item));
  }
  for (const auto &[Number, Entries] : Functions) {
    for (uint32_t Index : Entries) {
      auto &Function = Result.Symbols.at(Index);
      const auto &Section = Image.Sections[Result.Sections.at(Number)];
      // A singleton COMDAT code section and its exact section-definition
      // length bound a function independently of any MC provenance.
      if (!Function.Size && Entries.size() == 1 && Function.Offset == 0 &&
          SectionLengths.contains(Number) &&
          SectionLengths.at(Number) == Section.Size)
        Function.Size = Section.Size;
      if ((Function.Size &&
           !rangeInBounds(Function.Offset, Function.Size, Section.Size)) ||
          (!Function.Size && Original.Arch == Arch::X86))
        return fail(
            "function needs an exact auxiliary or singleton-section extent: " +
            Function.Name);
    }
  }
  for (const auto &R : Result.Relocations)
    if (!Result.Symbols.contains(R.SymbolIndex))
      return fail("relocation names an auxiliary/invalid symbol index");
  return Result;
}

llvm::Error place(NativeCOFF &Object, uint64_t Base, uint64_t Alignment) {
  auto &Image = Object.Image;
  Image.BaseVA = Base;
  uint64_t Cursor = Base;
  for (Kind Group : {Kind::Code, Kind::ReadOnlyData, Kind::WritableData,
                     Kind::UninitializedData}) {
    if (Group == Kind::WritableData)
      Cursor = llvm::alignTo(Cursor, Alignment);
    for (auto &S : Image.Sections)
      if (S.Kind == Group) {
        Cursor = llvm::alignTo(Cursor, S.Alignment);
        if (Cursor < Base ||
            Cursor - Base >
                ::neverd::limits::backend::kCOFFPatchObjectMaxStorage ||
            S.Size > ::neverd::limits::backend::kCOFFPatchObjectMaxStorage -
                         (Cursor - Base))
          return fail("object placement overflows");
        S.VA = Cursor;
        S.Offset = Cursor - Base;
        Cursor += S.Size;
      }
  }
  Image.Bytes.resize(Cursor - Base, 0);
  for (auto &S : Image.Sections) {
    std::copy(S.ExternalBytes.begin(), S.ExternalBytes.end(),
              Image.Bytes.begin() + S.Offset);
    S.ExternalBytes.clear();
  }
  for (const auto &[Index, S] : Object.Symbols)
    if (S.Section > 0 && Object.Sections.contains(S.Section)) {
      const uint64_t VA =
          Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset;
      if (Object.Names.contains(S.Name))
        Image.SymbolAddrs[S.Name] = VA;
    }
  Image.Success = true;
  return llvm::Error::success();
}

// Multiple functions in one native section have no auxiliary TotalSize in
// ordinary LLVM COFF. A decoded runtime-function row supplies an independent
// exact extent; never infer it from the next symbol or fabricated MC tokens.
llvm::Error completeFunctionRanges(NativeCOFF &Object,
                                   const BinaryImage &Original,
                                   bool RequireUnwind) {
  auto &Image = Object.Image;
  std::map<uint64_t, uint64_t> RuntimeRanges;
  const unsigned RowSize = Original.Arch == Arch::X64 ? 12 : 8;
  auto Read = [&](uint32_t RVA, uint64_t Size) -> const uint8_t * {
    const uint64_t VA = Original.Base + RVA;
    if (VA < Image.BaseVA ||
        !rangeInBounds(VA - Image.BaseVA, Size, Image.Bytes.size()))
      return nullptr;
    return Image.Bytes.data() + VA - Image.BaseVA;
  };
  if (Original.Arch != Arch::X86)
    for (const auto &Section : Image.Sections) {
      if (!llvm::StringRef(Section.Name).starts_with(".pdata"))
        continue;
      if (Section.Size % RowSize)
        return fail("native runtime-function directory has a partial row");
      for (uint64_t I = 0; I < Section.Size; I += RowSize) {
        auto Row =
            decodeCOFFRuntimeFunction(llvm::ArrayRef<uint8_t>(Image.Bytes)
                                          .slice(Section.Offset + I, RowSize),
                                      Original.Arch, Read);
        if (!Row)
          return Row.takeError();
        if (!RuntimeRanges
                 .emplace(Original.Base + Row->Begin, Original.Base + Row->End)
                 .second)
          return fail("duplicate native runtime-function start");
      }
    }
  for (auto &[Index, Symbol] : Object.Symbols) {
    if (!Symbol.Function || Symbol.Section <= 0 ||
        !Object.Sections.contains(Symbol.Section))
      continue;
    const auto &Section = Image.Sections[Object.Sections.at(Symbol.Section)];
    const uint64_t Begin = Section.VA + Symbol.Offset;
    if (!Symbol.Size) {
      auto Row = RuntimeRanges.find(Begin);
      if (Row == RuntimeRanges.end() || Row->second <= Begin)
        return fail("function has no auxiliary/section/runtime extent: " +
                    Symbol.Name);
      Symbol.Size = Row->second - Begin;
    }
    if (!rangeInBounds(Symbol.Offset, Symbol.Size, Section.Size))
      return fail("native function extent leaves its code section");
    if (RequireUnwind && (!RuntimeRanges.contains(Begin) ||
                          RuntimeRanges.at(Begin) != Begin + Symbol.Size))
      return fail("hosted DynCode requires exact compiler unwind coverage "
                  "for every native function");
    Object.Evidence.Functions.push_back({Begin, Begin + Symbol.Size});
  }
  llvm::sort(Object.Evidence.Functions, [](const auto &A, const auto &B) {
    return A.BeginVA < B.BeginVA;
  });
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
resolve(const NativeCOFF &Object, uint32_t Index, const BinaryImage &Original,
        const std::map<std::string, SourcePatchBinding> &Bindings,
        bool Callable) {
  const auto &S = Object.Symbols.at(Index);
  if (S.Section > 0 && Object.Sections.contains(S.Section)) {
    const auto &Section = Object.Image.Sections[Object.Sections.at(S.Section)];
    if (Callable && (Section.Kind != Kind::Code || !S.Function))
      return fail("call target is not a native function");
    return Section.VA + S.Offset + (S.Thumb ? 1 : 0);
  }
  if (S.Section != 0)
    return fail("reference to absolute/nonallocated symbol: " + S.Name);
  if (auto B = Bindings.find(S.Name); B != Bindings.end()) {
    if (Callable && B->second.Kind != SourcePatchBindingKind::Function)
      return fail("call uses a data binding");
    const auto Mode = Original.instructionModeAt(B->second.OriginalVA);
    return B->second.OriginalVA +
           (Callable && Mode && *Mode == InstructionMode::Thumb ? 1 : 0);
  }
  // DLL-import fields explicitly name the existing IAT slot. Direct calls
  // require an independently authenticated original function or import stub.
  const llvm::StringRef ImportPrefix =
      Original.Arch == Arch::X86 ? "_imp_" : "__imp_";
  if (llvm::StringRef(S.Name).starts_with(ImportPrefix) && !Callable) {
    std::set<uint64_t> Slots;
    const auto Name = canonicalName(
        llvm::StringRef(S.Name).drop_front(ImportPrefix.size()), Original.Arch);
    for (const auto &I : Original.Imports)
      if (I.Name == Name && I.IATAddr)
        Slots.insert(I.IATAddr);
    if (Slots.size() == 1)
      return *Slots.begin();
  }
  std::set<uint64_t> Targets;
  for (const auto &S0 : Original.Symbols)
    if (canonicalName(S0.Name, Original.Arch) == S.Name && S0.IsFunc &&
        Original.hasAuthenticatedFunctionEntryAt(S0.Addr))
      Targets.insert(S0.Addr);
  for (const auto &[VA, Import] : Original.ImportStubIndices)
    if (Import < Original.Imports.size() &&
        Original.Imports[Import].Name == S.Name && Original.isCodeAddress(VA))
      Targets.insert(VA);
  if (!Callable || Targets.size() != 1)
    return fail("external symbol requires a binding or authenticated original "
                "callable: " +
                S.Name);
  const uint64_t VA = *Targets.begin();
  const auto Mode = Original.instructionModeAt(VA);
  return VA + (Mode && *Mode == InstructionMode::Thumb ? 1 : 0);
}

llvm::Error
relocate(NativeCOFF &Object, const BinaryImage &Original,
         const std::map<std::string, SourcePatchBinding> &Bindings) {
  const Arch A = Original.Arch;
  std::map<uint64_t, unsigned> Written;
  for (const auto &R : Object.Relocations) {
    if (!R.Type)
      continue; // COFF ABSOLUTE is an explicit padding record.
    const auto &Section = Object.Image.Sections[R.Section];
    const bool Absolute64 =
        (A == Arch::X64 && R.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR64) ||
        (A == Arch::AArch64 && R.Type == llvm::COFF::IMAGE_REL_ARM64_ADDR64);
    const bool Absolute32 =
        (A == Arch::X86 && R.Type == llvm::COFF::IMAGE_REL_I386_DIR32) ||
        (A == Arch::ARM && R.Type == llvm::COFF::IMAGE_REL_ARM_ADDR32);
    const bool RVA =
        (A == Arch::X64 && R.Type == llvm::COFF::IMAGE_REL_AMD64_ADDR32NB) ||
        (A == Arch::X86 && R.Type == llvm::COFF::IMAGE_REL_I386_DIR32NB) ||
        (A == Arch::ARM && R.Type == llvm::COFF::IMAGE_REL_ARM_ADDR32NB) ||
        (A == Arch::AArch64 && R.Type == llvm::COFF::IMAGE_REL_ARM64_ADDR32NB);
    const bool Mov =
        A == Arch::ARM && R.Type == llvm::COFF::IMAGE_REL_ARM_MOV32T;
    const bool Rel32 =
        (A == Arch::X86 && R.Type == llvm::COFF::IMAGE_REL_I386_REL32) ||
        (A == Arch::X64 && R.Type >= llvm::COFF::IMAGE_REL_AMD64_REL32 &&
         R.Type <= llvm::COFF::IMAGE_REL_AMD64_REL32_5);
    const bool Branch =
        (A == Arch::ARM && R.Type == llvm::COFF::IMAGE_REL_ARM_BRANCH24T) ||
        (A == Arch::AArch64 && R.Type == llvm::COFF::IMAGE_REL_ARM64_BRANCH26);
    const bool Page = A == Arch::AArch64 &&
                      R.Type == llvm::COFF::IMAGE_REL_ARM64_PAGEBASE_REL21;
    const bool LowAdd = A == Arch::AArch64 &&
                        R.Type == llvm::COFF::IMAGE_REL_ARM64_PAGEOFFSET_12A;
    const bool LowLoad = A == Arch::AArch64 &&
                         R.Type == llvm::COFF::IMAGE_REL_ARM64_PAGEOFFSET_12L;
    if (!Absolute64 && !Absolute32 && !RVA && !Mov && !Rel32 && !Branch &&
        !Page && !LowAdd && !LowLoad)
      return fail("unsupported native relocation " + llvm::Twine(R.Type));
    const unsigned Width = Absolute64 || Mov ? 8 : 4;
    if (!rangeInBounds(R.Offset, Width, Section.Size))
      return fail("native relocation exceeds its section");
    const uint64_t P = Section.VA + R.Offset;
    auto Next = Written.lower_bound(P);
    if ((Next != Written.end() && Next->first - P < Width) ||
        (Next != Written.begin() &&
         P - std::prev(Next)->first < std::prev(Next)->second))
      return fail("overlapping native relocation writers");
    Written.emplace(P, Width);
    const bool Callable =
        Branch || (Rel32 && R.Offset && Section.Kind == Kind::Code &&
                   (Object.Image.Bytes[Section.Offset + R.Offset - 1] == 0xe8 ||
                    Object.Image.Bytes[Section.Offset + R.Offset - 1] == 0xe9));
    auto Target = resolve(Object, R.SymbolIndex, Original, Bindings, Callable);
    if (!Target)
      return Target.takeError();
    auto *Out = Object.Image.Bytes.data() + Section.Offset + R.Offset;
    llvm::APInt V(128, *Target);
    auto Delta = [&](uint64_t Base, unsigned Bits) -> llvm::Expected<int64_t> {
      auto D = V - llvm::APInt(128, Base);
      if (!D.isSignedIntN(Bits))
        return fail("native relocation displacement overflows");
      return D.sextOrTrunc(64).getSExtValue();
    };
    if (Absolute64 || Absolute32 || RVA) {
      V += llvm::APInt(128, Absolute64 ? readLE<uint64_t>(Out)
                                       : readLE<uint32_t>(Out));
      if (RVA) {
        if (A == Arch::ARM)
          V.clearBit(0);
        V -= llvm::APInt(128, Original.Base);
      }
      if (!V.isIntN(Width * 8))
        return fail("native absolute/RVA relocation overflows");
      const uint64_t Value = V.getLoBits(64).getZExtValue();
      if (Width == 8)
        writeLE<uint64_t>(Out, Value);
      else
        writeLE<uint32_t>(Out, uint32_t(Value));
      if (!RVA)
        Object.Evidence.BaseRelocations.push_back(
            {P,
             Width == 8 ? unsigned(llvm::COFF::IMAGE_REL_BASED_DIR64)
                        : unsigned(llvm::COFF::IMAGE_REL_BASED_HIGHLOW),
             Width});
    } else if (Rel32) {
      V += llvm::APInt(128, uint64_t(int64_t(int32_t(readLE<uint32_t>(Out)))),
                       true);
      const unsigned Bias =
          A == Arch::X64 ? R.Type - llvm::COFF::IMAGE_REL_AMD64_REL32 : 0;
      auto D = Delta(P + 4 + Bias, 32);
      if (!D)
        return D.takeError();
      writeLE<uint32_t>(Out, uint32_t(*D));
    } else if (Mov) {
      const uint16_t L1 = readLE<uint16_t>(Out), L2 = readLE<uint16_t>(Out + 2),
                     H1 = readLE<uint16_t>(Out + 4),
                     H2 = readLE<uint16_t>(Out + 6);
      if ((L1 & 0xfbf0) != 0xf240 || (H1 & 0xfbf0) != 0xf2c0 || (L2 & 0x8000) ||
          (H2 & 0x8000) || (L2 & 0x0f00) != (H2 & 0x0f00))
        return fail("MOV32T is not an exact MOVW/MOVT register pair");
      auto Imm = [](uint16_t A, uint16_t B) {
        return ((A & 0xf) << 12) | ((A & 0x400) << 1) | ((B & 0x7000) >> 4) |
               (B & 0xff);
      };
      V += llvm::APInt(128, uint32_t(Imm(L1, L2) | (Imm(H1, H2) << 16)));
      if (!V.isIntN(32))
        return fail("MOV32T address overflows");
      const uint32_t Value = V.getLoBits(32).getZExtValue();
      auto Encode = [](uint8_t *P, uint16_t A, uint16_t B, uint16_t I) {
        writeLE<uint16_t>(P, (A & 0xfbf0) | (I >> 12) | ((I & 0x800) >> 1));
        writeLE<uint16_t>(P + 2,
                          (B & 0x8f00) | ((I & 0x700) << 4) | (I & 0xff));
      };
      Encode(Out, L1, L2, Value);
      Encode(Out + 4, H1, H2, Value >> 16);
      Object.Evidence.BaseRelocations.push_back(
          {P, llvm::COFF::IMAGE_REL_BASED_ARM_MOV32T, 8});
    } else if (A == Arch::ARM) {
      const uint16_t H = readLE<uint16_t>(Out), L = readLE<uint16_t>(Out + 2);
      if ((H & 0xf800) != 0xf000 || (L & 0xd000) != 0xd000 || !(*Target & 1))
        return fail("Thumb branch needs an unsupported mode or instruction");
      const unsigned S = (H >> 10) & 1, I1 = !(((L >> 13) & 1) ^ S),
                     I2 = !(((L >> 11) & 1) ^ S);
      const int64_t Addend =
          llvm::SignExtend64<25>((S << 24) | (I1 << 23) | (I2 << 22) |
                                 ((H & 0x3ff) << 12) | ((L & 0x7ff) << 1));
      V.clearBit(0);
      V += llvm::APInt(128, uint64_t(Addend), true);
      auto D = Delta(P + 4, 25);
      if (!D)
        return D.takeError();
      if (*D & 1)
        return fail("unaligned Thumb branch");
      const uint32_t I = uint32_t(*D) & 0x1ffffff, Sign = I >> 24;
      writeLE<uint16_t>(Out, 0xf000 | (Sign << 10) | ((I >> 12) & 0x3ff));
      writeLE<uint16_t>(Out + 2, (L & 0xd000) |
                                     ((!(((I >> 23) & 1) ^ Sign)) << 13) |
                                     ((!(((I >> 22) & 1) ^ Sign)) << 11) |
                                     ((I >> 1) & 0x7ff));
    } else {
      const uint32_t Old = readLE<uint32_t>(Out);
      if (Branch) {
        if ((Old & 0x7c000000) != 0x14000000)
          return fail("BRANCH26 does not name B/BL");
        V += llvm::APInt(
            128, uint64_t(llvm::SignExtend64<26>(Old & 0x3ffffff) * 4), true);
        auto D = Delta(P, 28);
        if (!D)
          return D.takeError();
        if (*D & 3)
          return fail("unaligned ARM64 branch");
        writeLE<uint32_t>(Out, (Old & 0xfc000000) |
                                   ((uint32_t(*D) >> 2) & 0x3ffffff));
      } else if (Page) {
        if ((Old & 0x9f000000) != 0x90000000)
          return fail("PAGEBASE does not name ADRP");
        const int64_t Addend =
            llvm::SignExtend64<21>(((Old >> 5) & 0x7ffff) << 2 |
                                   ((Old >> 29) & 3)) *
            4096;
        V += llvm::APInt(128, uint64_t(Addend), true);
        V = V & ~llvm::APInt(128, 4095);
        auto D = Delta(P & ~uint64_t(4095), 33);
        if (!D)
          return D.takeError();
        const uint32_t I = uint32_t(*D >> 12) & 0x1fffff;
        writeLE<uint32_t>(Out, (Old & 0x9f00001f) | ((I & 3) << 29) |
                                   ((I >> 2) << 5));
      } else if (LowAdd) {
        if ((Old & 0x7fc00000) != 0x11000000)
          return fail("PAGEOFFSET_12A does not name unshifted ADD");
        const uint32_t I = uint32_t(*Target + ((Old >> 10) & 0xfff)) & 0xfff;
        writeLE<uint32_t>(Out, (Old & ~0x003ffc00) | (I << 10));
      } else {
        if ((Old & 0x3b000000) != 0x39000000)
          return fail("PAGEOFFSET_12L does not name unsigned load/store");
        unsigned Scale = (Old >> 30) & 3;
        if (!Scale && (Old & 0x04800000) == 0x04800000)
          Scale = 4; // The unsigned Q-register load/store scales by 16 bytes.
        const uint32_t I =
            uint32_t(*Target + (((Old >> 10) & 0xfff) << Scale)) & 0xfff;
        if (I & ((1u << Scale) - 1))
          return fail("unaligned page-relative load/store");
        writeLE<uint32_t>(Out, (Old & ~0x003ffc00) | ((I >> Scale) << 10));
      }
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<PatchResult> installCOFFObjectPatch(
    llvm::StringRef Bytes, const BinaryImage &OriginalInput,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> RequestedBindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const DynCodePatchEvidence *DynCode,
    const SourcePatchPreparation *Preparation) {
  const BinaryImage &Original =
      Preparation ? Preparation->Image : OriginalInput;
  auto Object = readObject(Bytes, Original);
  if (!Object)
    return Object.takeError();
  auto HeaderBytes = Original.Raw;
  auto PE = locatePEHeaders(HeaderBytes.data(), HeaderBytes.size());
  if (!PE.valid())
    return fail("original PE headers are invalid");
  std::set<std::string> Entries;
  std::set<uint64_t> Addresses;
  for (const auto &M : Mappings) {
    if (!Object->Names.contains(M.Symbol) || !Entries.insert(M.Symbol).second ||
        !Addresses.insert(M.OriginalVA).second ||
        !Original.hasAuthenticatedFunctionEntryAt(M.OriginalVA) ||
        !sourceEntryCanHostTrampoline(Original, M.OriginalVA))
      return fail("invalid explicit native entry mapping");
    const auto &S = Object->Symbols.at(Object->Names.at(M.Symbol));
    if (!S.Function || S.Section <= 0)
      return fail("entry is not a bounded native definition");
  }
  std::map<std::string, SourcePatchBinding> Bindings;
  BinaryImage References = Original;
  scanImportThunks(References);
  for (const auto &B : RequestedBindings) {
    if (!Object->Names.contains(B.Symbol) ||
        Object->Symbols.at(Object->Names.at(B.Symbol)).Section != 0 ||
        !Bindings.emplace(B.Symbol, B).second)
      return fail("binding needs a unique native undefined symbol");
    if (auto E = validateSourcePatchBinding(B, References, Mappings))
      return std::move(E);
  }
  const uint64_t Alignment = getPESectionAlignment(PE);
  if (!llvm::isPowerOf2_64(Alignment) || Alignment < 4096 || Alignment > 65536)
    return fail("original PE section alignment is unsupported");
  const uint64_t Offset = llvm::alignTo(getPESizeOfImage(PE), Alignment);
  if (Offset > UINT64_MAX - Original.Base)
    return fail("original PE placement exceeds its address space");
  const uint64_t Base = Original.Base + Offset;
  if (auto E = place(*Object, Base, Alignment))
    return std::move(E);
  if (auto E = relocate(*Object, References, Bindings))
    return std::move(E);
  if (auto E = completeFunctionRanges(*Object, Original, DynCode != nullptr))
    return std::move(E);
  if (DynCode)
    if (auto E =
            validateDynCodePatchImage(*DynCode, Bytes, Original, Object->Image,
                                      Mappings, RequestedBindings))
      return std::move(E);
  return publishSourcePatch(
      OriginalInput, Mappings, Output,
      [&](const std::filesystem::path &Input,
          const std::filesystem::path &Candidate)
          -> llvm::Expected<PatchResult> {
        std::string Failure;
        auto R = BinaryPatcher::readPatchWrite(
            Input, Candidate, true, "object_patch",
            [&](std::vector<uint8_t> &Binary, PatchResult &Result) {
              // Entry offsets belong to the prepared input. Header growth may
              // move every section's raw storage, so write the authenticated
              // jumps before the format installer moves those bytes together.
              for (const auto &M : Mappings) {
                const auto Mode = Original.instructionModeAt(M.OriginalVA);
                const uint64_t Target = Object->Image.SymbolAddrs.at(M.Symbol);
                if (!Mode ||
                    (Original.Arch == Arch::ARM &&
                     *Mode != InstructionMode::Thumb) ||
                    !getTargetCodegenInfo(Original.Arch, *Mode)
                         .writeSourceTrampoline(
                             Binary, Original.vaToFileOffset(M.OriginalVA),
                             Target, M.OriginalVA,
                             getTargetCodegenInfo(Original.Arch, *Mode)
                                 .sourceTrampolineSize())) {
                  Failure = "native entry trampoline is out of range";
                  return false;
                }
                Result.PatchedFunctions.push_back(
                    {M.Symbol, M.OriginalVA, Target, M.Symbol});
                Result.PatchedOriginalEntries.push_back(M.OriginalVA);
              }
              if (auto E = installCOFFSourceImage(
                      Binary, Original, Object->Image, &Object->Evidence)) {
                Failure = llvm::toString(std::move(E));
                return false;
              }
              llvm::sort(Result.PatchedOriginalEntries);
              Result.TrampolineCount = Mappings.size();
              Result.CodeSize = Object->Image.Bytes.size();
              return true;
            });
        if (!R.Success)
          return fail(Failure.empty() ? "candidate installation failed"
                                      : Failure);
        return R;
      },
      PrepareReceipt, Preparation,
      [&](const BinaryImage &Final) -> llvm::Error {
        for (const auto &S : Object->Image.Sections)
          if (auto E =
                  validateInstalledSourcePatchSection(Final, Object->Image, S))
            return E;
        return llvm::Error::success();
      });
}
} // namespace neverd
