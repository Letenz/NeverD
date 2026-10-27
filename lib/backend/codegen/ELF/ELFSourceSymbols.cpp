//===- ELFSourceSymbols.cpp - Source replacement references -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFSourceSymbols.h"

#include "neverd/backend/codegen/ELF/ELFSourceRelocations.h"
#include "neverd/object/ELFLayout.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <set>

namespace neverd {

// GNU FEATURE_1_AND describes link-time compatibility, not a request to
// enable CET for a previously non-CET executable. Retain its bytes without
// changing the original image's property note. IBT images need a different
// entry trampoline that preserves ENDBR, and are refused below.
llvm::Expected<uint32_t> readELFPatchPropertyNote(llvm::ArrayRef<uint8_t> Bytes,
                                                  uint8_t PointerWidth) {
  if (PointerWidth != 4 && PointerWidth != 8)
    return llvm::createStringError(llvm::errc::not_supported,
                                   "invalid GNU property pointer width");
  if (Bytes.size() < 16 || readLE<uint32_t>(Bytes.data()) != 4 ||
      readLE<uint32_t>(Bytes.data() + 8) != llvm::ELF::NT_GNU_PROPERTY_TYPE_0 ||
      std::memcmp(Bytes.data() + 12, "GNU\0", 4) != 0 ||
      readLE<uint32_t>(Bytes.data() + 4) != Bytes.size() - 16)
    return llvm::createStringError(llvm::errc::not_supported,
                                   "malformed GNU property note");
  uint32_t Features = 0;
  std::set<uint32_t> Seen;
  for (uint64_t Offset = 16; Offset < Bytes.size();) {
    if (!rangeInBounds(Offset, 12, Bytes.size()))
      return llvm::createStringError(llvm::errc::not_supported,
                                     "truncated GNU property");
    const uint32_t Type = readLE<uint32_t>(Bytes.data() + Offset);
    const uint32_t Size = readLE<uint32_t>(Bytes.data() + Offset + 4);
    const uint32_t Value = readLE<uint32_t>(Bytes.data() + Offset + 8);
    if (Size != 4 || !Seen.insert(Type).second)
      return llvm::createStringError(llvm::errc::not_supported,
                                     "unsupported or duplicate GNU property");
    if (Type == llvm::ELF::GNU_PROPERTY_X86_FEATURE_1_AND && !(Value & ~3u))
      Features = Value;
    else if (Type != llvm::ELF::GNU_PROPERTY_X86_ISA_1_NEEDED || Value != 1)
      return llvm::createStringError(
          llvm::errc::not_supported,
          "GNU property requires an unsupported CPU/runtime feature");
    Offset = llvm::alignTo(Offset + 8 + Size, PointerWidth);
    if (Offset > Bytes.size())
      return llvm::createStringError(llvm::errc::not_supported,
                                     "invalid GNU property alignment");
  }
  return Features;
}

llvm::Expected<uint32_t>
readELFX64PatchPropertyNote(llvm::ArrayRef<uint8_t> Bytes) {
  return readELFPatchPropertyNote(Bytes, 8);
}

namespace {
template <typename ELF>
llvm::Error validateX86SourceProperties(const BinaryImage &Original) {
  auto OriginalELF = llvm::object::ELFFile<ELF>::create(
      llvm::StringRef(reinterpret_cast<const char *>(Original.Raw.data()),
                      Original.Raw.size()));
  if (!OriginalELF)
    return OriginalELF.takeError();
  auto OriginalSections = OriginalELF->sections();
  if (!OriginalSections)
    return OriginalSections.takeError();
  for (const auto &Section : *OriginalSections) {
    auto Name = OriginalELF->getSectionName(Section);
    if (!Name)
      return Name.takeError();
    if (*Name != ".note.gnu.property")
      continue;
    auto Note = OriginalELF->getSectionContents(Section);
    if (!Note)
      return Note.takeError();
    auto Features = readELFPatchPropertyNote(*Note, sizeof(typename ELF::Addr));
    if (!Features)
      return Features.takeError();
    if (Original.Arch == Arch::X86 &&
        (*Features & llvm::ELF::GNU_PROPERTY_X86_FEATURE_1_SHSTK))
      return llvm::createStringError(
          llvm::errc::not_supported,
          "i386 shadow-stack images require a shadow-stack-safe import thunk");
    if (*Features & llvm::ELF::GNU_PROPERTY_X86_FEATURE_1_IBT)
      return llvm::createStringError(
          llvm::errc::not_supported,
          "IBT original entries require an ENDBR-preserving trampoline");
  }
  return llvm::Error::success();
}
} // namespace

llvm::Error validateELFSourceFeatures(const BinaryImage &Original) {
  if (Original.Format != BinaryFormat::ELF)
    return llvm::Error::success();
  if (Original.Arch == Arch::ARM) {
    bool Dynamic = false;
    forEachELFPhdr(Original.Raw.data(), Original.Raw.size(),
                   [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                     Dynamic |= P.Type == llvm::ELF::PT_DYNAMIC;
                   });
    if (!Dynamic)
      for (const auto &S : Original.Symbols)
        if (Original.isCodeAddress(S.Addr) &&
            (S.Name == "__gnu_Unwind_Find_exidx" ||
             S.Name == "_Unwind_RaiseException" ||
             llvm::StringRef(S.Name).starts_with("__aeabi_unwind_cpp_pr")))
          return llvm::createStringError(
              llvm::errc::not_supported,
              "static ARM unwinder may retain a baked exidx registry; "
              "replacement requires dynamic PT_ARM_EXIDX discovery");
    return llvm::Error::success();
  }
  if (Original.Arch == Arch::X86)
    return validateX86SourceProperties<llvm::object::ELF32LE>(Original);
  if (Original.Arch == Arch::X64)
    return validateX86SourceProperties<llvm::object::ELF64LE>(Original);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>> resolveELFSourceSymbol(
    const BinaryImage &Image, const llvm::Module &Module,
    const llvm::mc_rewrite::RewriteSymbolResolveRequest &Request) {
  auto Fail = [&](const llvm::Twine &Detail) {
    return llvm::createStringError(llvm::errc::not_supported,
                                   "ELF source reference '" + Request.Symbol +
                                       "': " + Detail);
  };
  if (!Image.isELF() || Request.Symbol.empty() || Request.IsSubtrahend)
    return Fail("unsupported image or subtractive external reference");
  const llvm::StringRef Specifier = Request.SpecifierName;
  const bool AArch64Call = Image.Arch == Arch::AArch64 &&
                           Request.Specifier == 1 &&
                           (Request.FixupKindName.contains("call26") ||
                            Request.FixupKindName.contains("branch26"));
  const bool AArch64Absolute =
      Image.Arch == Arch::AArch64 &&
      ((Request.Specifier == 0x001 && Specifier == "ABS") ||
       (Request.Specifier == 0x011 && Specifier == "ABS_PAGE") ||
       (Request.Specifier == 0x121 && Specifier == "ABS_LO12"));
  const bool Slot = Specifier == "GOTPCREL" ||
                    Specifier == "GOTPCREL_NORELAX" || Specifier == "GOT" ||
                    Specifier == "GOT_PAGE" || Specifier == "GOT_LO12";
  const bool LocalGOTOffset =
      Image.Arch == Arch::X86 && Specifier == "GOTOFF" &&
      Request.Specifier == 10 && !Request.IsPCRel && Request.BitWidth == 32 &&
      parseNdDataSymbol(Request.Symbol).has_value();
  if (!Slot && Specifier != "PLT" &&
      !(Request.Specifier == 0 && Specifier.empty()) && !AArch64Call &&
      !AArch64Absolute && !LocalGOTOffset)
    return Fail("unsupported relocation specifier: " + Specifier + " (" +
                llvm::Twine(Request.Specifier) + ", " + Request.FixupKindName +
                ", " + Request.SectionName + ")");

  if (auto VA = parseNdDataSymbol(Request.Symbol)) {
    const auto *Declaration = Module.getNamedValue(Request.Symbol);
    if (Slot || !Declaration || !Declaration->isDeclaration() ||
        !Declaration->isDSOLocal())
      return Fail("original-address binding cannot serve as an import slot");
    if (llvm::isa<llvm::Function>(Declaration)) {
      if (!Image.hasAuthenticatedFunctionEntryAt(*VA) &&
          !Image.findImportStubAt(*VA))
        return Fail("bound function has no authenticated entry");
      const auto Mode = Image.instructionModeAt(*VA);
      if (!Mode)
        return Fail("bound function has no instruction mode");
      return std::optional<uint64_t>(
          serializeCodePointer(*VA, Image.Arch, *Mode));
    }
    if (!Image.isDataAddress(*VA))
      return Fail("bound data has no mapped storage");
    return std::optional<uint64_t>(*VA);
  }

  // The runtime address of an imported personality/function pointer is not
  // known while patching. Its absolute storage field is deliberately deferred
  // to an authenticated existing dynamic symbol. prepareELFSourceRelocations
  // must register this exact final MC fixup before the candidate can publish.
  if ((Image.Arch == Arch::X64 || Image.Arch == Arch::AArch64) &&
      (Request.Specifier == 0 ||
       (Request.Specifier == 1 && Specifier == "ABS")) &&
      !Request.IsPCRel && Request.BitWidth == 64 &&
      Request.SectionName.starts_with(".data")) {
    auto Symbol = findELFSourceDynamicSymbol(Image, Request.Symbol);
    if (!Symbol)
      return Symbol.takeError();
    if (*Symbol)
      return std::optional<uint64_t>(0);
  }

  std::set<uint64_t> Candidates;
  if (Slot) {
    // Only an eagerly resolved GLOB_DAT slot has the ordinary GOT contract.
    // A lazy JUMP_SLOT may still contain a PLT resolver address, so it cannot
    // stand in for arbitrary data or a canonical imported function pointer.
    uint32_t Type = 0;
    switch (Image.Arch) {
    case Arch::X64:
      Type = llvm::ELF::R_X86_64_GLOB_DAT;
      break;
    case Arch::X86:
      Type = llvm::ELF::R_386_GLOB_DAT;
      break;
    case Arch::AArch64:
      Type = llvm::ELF::R_AARCH64_GLOB_DAT;
      break;
    case Arch::ARM:
      Type = llvm::ELF::R_ARM_GLOB_DAT;
      break;
    default:
      return Fail("unsupported GOT architecture");
    }
    const uint64_t Width = Image.getPointerSize();
    for (const auto &Relocation : Image.Relocations) {
      if (Relocation.SymbolName != Request.Symbol || Relocation.Type != Type)
        continue;
      const uint64_t VA = Relocation.Address;
      const auto *Segment = Image.getSegmentFor(VA);
      if (Relocation.Addend || !Width || VA % Width || !Segment ||
          !Segment->isReadable() || !Image.isDataAddress(VA) ||
          !rangeInBounds(VA - Segment->VA, Width, Segment->Size) ||
          !Image.readVA(VA, Width))
        return Fail("GOT relocation has invalid storage or addend");
      for (const auto &Other : Image.Relocations)
        if (&Other != &Relocation && Other.Address == VA &&
            (Other.Type != Type || Other.SymbolName != Request.Symbol ||
             Other.Addend != 0))
          return Fail("conflicting loader relocations at GOT slot");
      Candidates.insert(VA);
    }
  } else {
    // No implicit data-symbol copies or address guesses: explicit bindings
    // carry the declaration's extent, alignment and mutability checks.
    const bool CompilerCall =
        Specifier == "PLT" ||
        Request.FixupKindName.contains_insensitive("branch") ||
        Request.FixupKindName.contains_insensitive("call");
    if (!Module.getFunction(Request.Symbol) && !CompilerCall)
      return Fail("direct data reference requires an explicit binding");
    for (const auto &Symbol : Image.Symbols)
      if (Symbol.Name == Request.Symbol && Symbol.IsFunc &&
          Image.hasAuthenticatedFunctionEntryAt(Symbol.Addr))
        Candidates.insert(Symbol.Addr);
    for (const auto &Export : Image.Exports)
      if (Export.Name == Request.Symbol &&
          Image.hasAuthenticatedFunctionEntryAt(Export.Addr))
        Candidates.insert(Export.Addr);
    for (const auto &[VA, Index] : Image.ImportStubIndices)
      if (Index < Image.Imports.size() &&
          Image.Imports[Index].Name == Request.Symbol &&
          Image.findImportStubAt(VA) && Image.isCodeAddress(VA))
        Candidates.insert(VA);
  }
  if (Candidates.size() > 1)
    return Fail("ambiguous original symbol");
  if (Candidates.empty())
    return std::optional<uint64_t>();
  const uint64_t VA = *Candidates.begin();
  if (Slot)
    return std::optional<uint64_t>(VA);
  const auto Mode = Image.instructionModeAt(VA);
  if (!Mode)
    return Fail("function has no instruction mode");
  return std::optional<uint64_t>(serializeCodePointer(VA, Image.Arch, *Mode));
}

} // namespace neverd
