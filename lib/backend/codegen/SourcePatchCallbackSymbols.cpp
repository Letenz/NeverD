//===- SourcePatchCallbackSymbols.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register callback symbol and mode identities.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFSectionPatch.h"
#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/MachOLayout.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {
llvm::Error fail(const llvm::Twine &S) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "callback symbols: " + S);
}
std::string symbol(const SourceCallbackPrefix &P) {
  return "__neverd_original_" + llvm::utohexstr(P.CallbackVA);
}
llvm::Error elf(std::vector<uint8_t> &B,
                llvm::ArrayRef<SourceCallbackPrefix> P) {
  using ELF = llvm::object::ELF32LE;
  ELFHeaderInfo H;
  if (!validateELFHeaderTables(B.data(), B.size(), H) || H.Is64)
    return fail("expected validated ELF32 tables");
  std::vector<CompiledSection> Sections;
  for (const auto &F : P) {
    CompiledSection S;
    S.Name = ".original." + llvm::utohexstr(F.CallbackVA);
    S.VA = F.CallbackVA;
    S.Size = F.Bytes.size();
    S.Alignment = 2;
    S.Kind = llvm::mc_rewrite::RewriteSectionKind::Code;
    S.IsAllocated = S.IsInImage = true;
    Sections.push_back(S);
  }
  if (auto E = registerELFPatchSections(B, Sections))
    return E;
  H = parseELFHeader(B.data(), B.size());
  std::vector<ELFShdrFields> Headers;
  std::optional<size_t> Table;
  bool Bad = false;
  forEachELFShdr(B.data(), B.size(), [&](const auto &S, uint16_t N) {
    Headers.push_back(S);
    if (S.Type == llvm::ELF::SHT_SYMTAB) {
      Bad |= Table.has_value();
      Table = N;
    }
  });
  if (Bad || !Table)
    return fail("callback needs one native symbol table");
  auto &T = Headers[*Table];
  if (T.Link >= Headers.size() || T.EntSize != sizeof(ELF::Sym) ||
      T.Size % T.EntSize || (T.Flags & llvm::ELF::SHF_ALLOC) ||
      !rangeInBounds(T.Offset, T.Size, B.size()))
    return fail("unmodeled ELF symbol table");
  auto &Names = Headers[T.Link];
  if (Names.Type != llvm::ELF::SHT_STRTAB ||
      (Names.Flags & llvm::ELF::SHF_ALLOC) || !Names.Size ||
      !rangeInBounds(Names.Offset, Names.Size, B.size()) || B[Names.Offset] ||
      B[Names.Offset + Names.Size - 1])
    return fail("unmodeled ELF string table");
  std::vector<uint8_t> Syms(B.begin() + T.Offset,
                            B.begin() + T.Offset + T.Size);
  std::vector<uint8_t> Strings(B.begin() + Names.Offset,
                               B.begin() + Names.Offset + Names.Size);
  for (size_t I = 0; I < P.size(); ++I) {
    if (P[I].CallbackVA > UINT32_MAX || P[I].Bytes.size() > UINT32_MAX ||
        Strings.size() > UINT32_MAX)
      return fail("ELF callback extent overflows");
    const auto N = symbol(P[I]);
    ELF::Sym S{};
    S.st_name = Strings.size();
    S.st_info = (llvm::ELF::STB_GLOBAL << 4) | llvm::ELF::STT_FUNC;
    S.st_other = llvm::ELF::STV_HIDDEN;
    S.st_shndx = H.ShNum - P.size() + I;
    S.st_value =
        P[I].CallbackVA | (P[I].Mode == InstructionMode::Thumb ? 1 : 0);
    S.st_size = P[I].Bytes.size();
    Strings.insert(Strings.end(), N.begin(), N.end());
    Strings.push_back(0);
    const auto *Raw = reinterpret_cast<const uint8_t *>(&S);
    Syms.insert(Syms.end(), Raw, Raw + sizeof(S));
  }
  if (B.size() + Syms.size() + Strings.size() + 8 > UINT32_MAX)
    return fail("ELF callback tables exceed file limits");
  B.resize(llvm::alignTo(B.size(), 4), 0);
  T.Offset = B.size();
  T.Size = Syms.size();
  B.insert(B.end(), Syms.begin(), Syms.end());
  Names.Offset = B.size();
  Names.Size = Strings.size();
  B.insert(B.end(), Strings.begin(), Strings.end());
  writeELFShdr(B.data() + H.ShOff + *Table * H.ShEntSize, false, T);
  writeELFShdr(B.data() + H.ShOff + T.Link * H.ShEntSize, false, Names);
  return llvm::Error::success();
}
llvm::Error macho(std::vector<uint8_t> &B,
                  llvm::ArrayRef<SourceCallbackPrefix> P) {
  using namespace llvm::MachO;
  const auto H = parseMachOHeader(B.data(), B.size());
  const unsigned SS = sizeof(section);
  const uint64_t End = H.HeaderSize + uint64_t(H.SizeOfCmds);
  const uint64_t First = findFirstMachODataOffset(B.data(), B.size());
  if (H.Is64 || !H.HeaderSize || End + SS > First ||
      !rangeInBounds(End, SS, B.size()) ||
      llvm::any_of(llvm::ArrayRef<uint8_t>(B).slice(End, SS),
                   [](uint8_t C) { return C != 0; }))
    return fail("callback mode section needs authenticated command capacity");
  uint64_t Code = 0, Sym = 0, Dyn = 0, LE = 0;
  unsigned Ordinal = 0, CodeOrdinal = 0;
  bool Bad = false;
  forEachMachOLoadCommand(
      B.data(), B.size(),
      [&](const uint8_t *C, uint32_t ID, uint32_t Size, bool) {
        if (ID == LC_SYMTAB) {
          Bad |= Sym != 0;
          Sym = C - B.data();
        }
        if (ID == LC_DYSYMTAB) {
          Bad |= Dyn != 0;
          Dyn = C - B.data();
        }
        if (ID != LC_SEGMENT)
          return;
        const auto F = readMachOSegment(C, false);
        Bad |= Size != sizeof(segment_command) + F.NSects * sizeof(section);
        if (Code && F.NSects)
          Bad = true; // No following section ordinals can move.
        Ordinal += F.NSects;
        if (readMachOName(F.SegName) == "__NDORIG") {
          Bad |= Code != 0 || F.NSects != 0 || F.InitProt != 5;
          Code = C - B.data();
          CodeOrdinal = Ordinal + 1;
          for (const auto &R : P)
            Bad |= R.CallbackVA < F.VMAddr ||
                   !rangeInBounds(R.CallbackVA - F.VMAddr, R.Bytes.size(),
                                  F.FileSize);
        }
        if (readMachOName(F.SegName) == "__LINKEDIT") {
          Bad |= LE != 0 || F.FileOff + F.FileSize != B.size();
          LE = C - B.data();
        }
      });
  if (Bad || !Code || !Sym || !LE || CodeOrdinal > 255)
    return fail("callback segment or native symbol ownership is ambiguous");
  auto T = *reinterpret_cast<const symtab_command *>(B.data() + Sym);
  dysymtab_command D{};
  uint64_t InsertSymbol = T.nsyms;
  if (Dyn) {
    D = *reinterpret_cast<const dysymtab_command *>(B.data() + Dyn);
    InsertSymbol = uint64_t(D.iextdefsym) + D.nextdefsym;
    if (D.ntoc || D.nmodtab || D.nextrefsyms || D.nextrel || D.nlocrel ||
        uint64_t(D.ilocalsym) + D.nlocalsym > D.iextdefsym ||
        InsertSymbol > D.iundefsym ||
        uint64_t(D.iundefsym) + D.nundefsym > T.nsyms ||
        !rangeInBounds(D.indirectsymoff, uint64_t(D.nindirectsyms) * 4,
                       B.size()))
      return fail("symbol insertion needs unmodeled dynamic indices");
  }
  if (InsertSymbol > T.nsyms || T.nsyms > UINT32_MAX - P.size() || !T.strsize ||
      !rangeInBounds(T.symoff, uint64_t(T.nsyms) * sizeof(nlist), B.size()) ||
      !rangeInBounds(T.stroff, T.strsize, B.size()) || B[T.stroff] ||
      B[T.stroff + T.strsize - 1])
    return fail("invalid original Mach-O symbol table");
  std::vector<uint8_t> Names(B.begin() + T.stroff,
                             B.begin() + T.stroff + T.strsize);
  std::vector<uint8_t> Syms(B.begin() + T.symoff,
                            B.begin() + T.symoff +
                                InsertSymbol * sizeof(nlist));
  for (const auto &R : P) {
    if (R.Mode != InstructionMode::Thumb || R.CallbackVA > UINT32_MAX ||
        Names.size() > UINT32_MAX)
      return fail("Mach-O callback requires exact Thumb mode");
    const auto N = "_" + symbol(R);
    nlist S{};
    S.n_strx = Names.size();
    S.n_type = N_SECT | N_EXT;
    S.n_sect = CodeOrdinal;
    S.n_desc = N_ARM_THUMB_DEF;
    S.n_value = R.CallbackVA;
    const auto *Raw = reinterpret_cast<const uint8_t *>(&S);
    Syms.insert(Syms.end(), Raw, Raw + sizeof(S));
    Names.insert(Names.end(), N.begin(), N.end());
    Names.push_back(0);
  }
  Syms.insert(Syms.end(), B.begin() + T.symoff + InsertSymbol * sizeof(nlist),
              B.begin() + T.symoff + uint64_t(T.nsyms) * sizeof(nlist));
  if (Dyn) {
    for (uint32_t I = 0; I < D.nindirectsyms; ++I) {
      auto *At = B.data() + D.indirectsymoff + uint64_t(I) * 4;
      const auto N = readLE<uint32_t>(At);
      if (N & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
        continue;
      if (N >= T.nsyms)
        return fail("indirect symbol index exceeds the old table");
      if (N >= InsertSymbol)
        writeLE<uint32_t>(At, N + P.size());
    }
    D.nextdefsym += P.size();
    D.iundefsym += P.size();
    std::memcpy(B.data() + Dyn, &D, sizeof(D));
  }
  if (B.size() + Syms.size() + Names.size() + 16 > UINT32_MAX)
    return fail("Mach-O callback tables exceed file limits");
  B.resize(llvm::alignTo(B.size(), 4), 0);
  T.symoff = B.size();
  T.nsyms += P.size();
  B.insert(B.end(), Syms.begin(), Syms.end());
  T.stroff = B.size();
  T.strsize = Names.size();
  B.insert(B.end(), Names.begin(), Names.end());
  std::memcpy(B.data() + Sym, &T, sizeof(T));
  auto Linkedit = readMachOSegment(B.data() + LE, false);
  Linkedit.FileSize = B.size() - Linkedit.FileOff;
  Linkedit.VMSize = llvm::alignTo(Linkedit.FileSize, 4096);
  if (Linkedit.VMAddr + Linkedit.VMSize > UINT32_MAX)
    return fail("LINKEDIT extent overflows");
  writeMachOSegment(B.data() + LE, false, Linkedit);
  auto F = readMachOSegment(B.data() + Code, false);
  const uint64_t Insert = Code + sizeof(segment_command);
  std::memmove(B.data() + Insert + SS, B.data() + Insert, End - Insert);
  std::memset(B.data() + Insert, 0, SS);
  auto *S = reinterpret_cast<section *>(B.data() + Insert);
  std::memcpy(S->sectname, "__text", 6);
  std::memcpy(S->segname, "__NDORIG", 8);
  S->addr = F.VMAddr;
  S->offset = F.FileOff;
  S->size = P.back().CallbackVA + P.back().Bytes.size() - F.VMAddr;
  S->align = 1;
  S->flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
  auto *G = reinterpret_cast<segment_command *>(B.data() + Code);
  G->cmdsize += SS;
  G->nsects = 1;
  setMachOHeaderCmds(B.data(), false, H.NCmds, H.SizeOfCmds + SS);
  return llvm::Error::success();
}
} // namespace
llvm::Error
registerSourceCallbackSymbols(std::vector<uint8_t> &Binary, BinaryFormat Format,
                              llvm::ArrayRef<SourceCallbackPrefix> Prefixes) {
  if (Prefixes.empty())
    return llvm::Error::success();
  auto Candidate = Binary;
  auto E = Format == BinaryFormat::ELF ? elf(Candidate, Prefixes)
           : Format == BinaryFormat::MachO
               ? macho(Candidate, Prefixes)
               : fail("unsupported native symbol format");
  if (E)
    return E;
  Binary.swap(Candidate);
  return llvm::Error::success();
}
} // namespace neverd
