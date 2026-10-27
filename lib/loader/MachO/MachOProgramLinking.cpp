//===- MachOProgramLinking.cpp - Complete execution linking facts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Strict original-byte decoding of supported Mach-O pointer fixups.
#include "neverd/loader/MachO/MachOProgramLinking.h"

#include "neverd/loader/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace neverd {
namespace {
using namespace llvm::MachO;
using namespace llvm::support::endian;
llvm::Error failure(llvm::Twine Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "Mach-O program linking: " + Message);
}
class Reader {
  llvm::ArrayRef<uint8_t> Bytes;
  uint64_t Limit, RecordLimit;
  llvm::function_ref<llvm::Error()> CheckPreparation;
  std::vector<segment_command_64> Segments;
  std::map<uint64_t, size_t> Slots;
  using Identity = std::tuple<std::string, int32_t, bool>;
  std::set<Identity> Undefined, Bound;
  uint64_t HeaderAddress = 0;
  uint64_t RawFixups = 0;

public:
  MachOProgramLinking Out;
  Reader(llvm::ArrayRef<uint8_t> Bytes, uint64_t Limit,
         llvm::function_ref<llvm::Error()> CheckPreparation)
      : Bytes(Bytes), Limit(Limit),
        RecordLimit(std::min<uint64_t>(
            ::neverd::limits::loader::kMachOProgramLinkingReaderMinArgument1,
            Bytes.size() / 8)),
        CheckPreparation(CheckPreparation) {}
  llvm::Error check() { return CheckPreparation(); }
  llvm::Error charge(uint64_t Count) {
    if (auto E = check())
      return E;
    if (Count > Limit - Out.MetadataBytes)
      return failure("decoded metadata exceeds its byte limit");
    Out.MetadataBytes += Count;
    return llvm::Error::success();
  }
  llvm::Expected<llvm::ArrayRef<uint8_t>> bytes(uint64_t Offset,
                                                uint64_t Size) {
    if (Offset > Bytes.size() || Size > Bytes.size() - Offset)
      return failure("metadata escapes the original file");
    return Bytes.slice(Offset, Size);
  }
  llvm::Expected<std::string> string(llvm::ArrayRef<uint8_t> Data,
                                     uint64_t Offset) {
    if (Offset >= Data.size())
      return failure("invalid string offset");
    const auto Text =
        llvm::StringRef(reinterpret_cast<const char *>(Data.data() + Offset),
                        Data.size() - Offset);
    const auto End = Text.find('\0');
    if (End == llvm::StringRef::npos || !End)
      return failure("empty or unterminated symbol/provider");
    if (auto E = charge(2 * End + 128))
      return std::move(E);
    return Text.take_front(End).str();
  }
  llvm::Expected<uint64_t> slot(unsigned Segment, uint64_t Offset) {
    if (Segment >= Segments.size())
      return failure("invalid fixup segment index");
    const auto &S = Segments[Segment];
    if (Offset > S.filesize || 8 > S.filesize - Offset || Offset > S.vmsize ||
        8 > S.vmsize - Offset || !(S.maxprot & VM_PROT_WRITE) ||
        ((S.maxprot | S.initprot) & VM_PROT_EXECUTE) || (S.vmaddr + Offset) % 8)
      return failure("fixup requires aligned non-executable file storage");
    return S.vmaddr + Offset;
  }
  llvm::Error add(MachOProgramFixup Fixup) {
    if (RawFixups >= RecordLimit)
      return failure("too many fixup records");
    if (auto E = charge(2 * sizeof(Fixup) + 160))
      return E;
    ++RawFixups;
    if (Fixup.Bind) {
      if (Fixup.LibraryOrdinal <= 0 ||
          uint64_t(Fixup.LibraryOrdinal) > Out.Dependencies.size())
        return failure("binding requires an ordinary dependency ordinal");
      if (auto E = charge(2 * Fixup.Symbol.size() + 160))
        return E;
      Identity ID{Fixup.Symbol, Fixup.LibraryOrdinal, Fixup.WeakImport};
      if (!Undefined.contains(ID))
        return failure("binding disagrees with undefined symbol metadata");
      Bound.insert(std::move(ID));
    } else {
      bool Mapped = false;
      for (const auto &S : Segments)
        Mapped |= S.initprot && Fixup.Target >= S.vmaddr &&
                  Fixup.Target - S.vmaddr < S.vmsize;
      if (!Mapped)
        return failure("rebase target is not a local mapped address");
    }
    if (auto Previous = Slots.find(Fixup.Address); Previous != Slots.end()) {
      auto &Original = Out.Fixups[Previous->second];
      if (Original.Origin != MachOProgramFixupOrigin::ClassicRebase ||
          Original.Bind || !Fixup.Bind ||
          Fixup.Origin != MachOProgramFixupOrigin::ClassicLazyBind)
        return failure("duplicate or overlapping fixup destination");
      const bool Executable = llvm::any_of(Segments, [&](const auto &S) {
        return (S.initprot & VM_PROT_EXECUTE) && Original.Target >= S.vmaddr &&
               Original.Target - S.vmaddr < S.filesize;
      });
      if (!Executable)
        return failure("lazy rebase target is not local executable bytes");
      Original = std::move(Fixup);
      return llvm::Error::success();
    }
    Slots.emplace(Fixup.Address, Out.Fixups.size());
    Out.Fixups.push_back(std::move(Fixup));
    return llvm::Error::success();
  }
  llvm::Expected<uint64_t> uleb(const uint8_t *&P, const uint8_t *End) {
    unsigned Size = 0;
    const char *Error = nullptr;
    const auto Value = llvm::decodeULEB128(P, &Size, End, &Error);
    if (Error || !Size)
      return failure("invalid ULEB128");
    P += Size;
    return Value;
  }
  llvm::Error stream(llvm::ArrayRef<uint8_t> Data, bool Bind) {
    const auto *P = Data.begin(), *End = Data.end();
    // Ordinary streams start with an unset type. Lazy programs have their own
    // implicit pointer type and independent per-program state below.
    unsigned Segment = UINT32_MAX, Type = 0;
    uint64_t Offset = 0;
    int32_t Ordinal = 0;
    std::string Name;
    int64_t Addend = 0;
    bool Weak = false;
    auto Number = [&](uint64_t &Value) -> llvm::Error {
      auto N = uleb(P, End);
      if (!N)
        return N.takeError();
      Value = *N;
      return llvm::Error::success();
    };
    auto Emit = [&](uint64_t Count, uint64_t Skip) -> llvm::Error {
      if (Type != 1 || Count > RecordLimit - RawFixups ||
          Skip > UINT64_MAX - 8 ||
          (Count && (Count - 1) > (UINT64_MAX - Offset) / (8 + Skip)))
        return failure(
            "unsupported pointer type or excessive stream expansion");
      for (uint64_t I = 0; I < Count; ++I) {
        auto Address = slot(Segment, Offset);
        if (!Address)
          return Address.takeError();
        MachOProgramFixup F;
        F.Address = *Address;
        F.Bind = Bind;
        F.Origin = Bind ? MachOProgramFixupOrigin::ClassicBind
                        : MachOProgramFixupOrigin::ClassicRebase;
        if (Bind) {
          if (Name.empty())
            return failure("binding has no symbol");
          if (auto E = charge(2 * Name.size() + 64))
            return E;
          F.Symbol = Name;
          F.LibraryOrdinal = Ordinal;
          F.Addend = Addend;
          F.WeakImport = Weak;
        } else
          F.Target =
              read64le(Bytes.data() + Segments[Segment].fileoff + Offset);
        if (auto E = add(std::move(F)))
          return E;
        if (8 + Skip > UINT64_MAX - Offset)
          return failure("stream offset overflows");
        Offset += 8 + Skip;
      }
      return llvm::Error::success();
    };
    while (P < End) {
      if (auto E = check())
        return E;
      const uint8_t Byte = *P++, Op = Byte & 0xf0, Imm = Byte & 0xf;
      if (!Op) {
        if (Byte || llvm::any_of(llvm::ArrayRef(P, End),
                                 [](uint8_t B) { return B != 0; }))
          return failure("non-padding bytes after stream terminator");
        return llvm::Error::success();
      }
      if (Bind) {
        switch (Op) {
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
          Ordinal = Imm;
          break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
          auto N = uleb(P, End);
          if (!N)
            return N.takeError();
          if (*N > INT32_MAX)
            return failure("dependency ordinal overflows");
          Ordinal = *N;
          break;
        }
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
          return failure("special dependency ordinals are unsupported");
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
          if (Imm & ~BIND_SYMBOL_FLAGS_WEAK_IMPORT)
            return failure("unsupported binding symbol flags");
          auto S = string(llvm::ArrayRef(P, End), 0);
          if (!S)
            return S.takeError();
          P += S->size() + 1;
          Name = std::move(*S);
          Weak = Imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT;
          break;
        }
        case BIND_OPCODE_SET_TYPE_IMM:
          Type = Imm;
          break;
        case BIND_OPCODE_SET_ADDEND_SLEB: {
          unsigned Size = 0;
          const char *Error = nullptr;
          Addend = llvm::decodeSLEB128(P, &Size, End, &Error);
          if (Error || !Size)
            return failure("invalid SLEB128");
          P += Size;
          break;
        }
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
          Segment = Imm;
          if (auto E = Number(Offset))
            return E;
          break;
        case BIND_OPCODE_ADD_ADDR_ULEB: {
          auto N = uleb(P, End);
          if (!N)
            return N.takeError();
          Offset += *N; // dyld's unsigned offset also permits backward moves.
          break;
        }
        case BIND_OPCODE_DO_BIND:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
          if (auto E = Emit(1, 0))
            return E;
          if (Op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) {
            auto N = uleb(P, End);
            if (!N)
              return N.takeError();
            Offset += *N;
          } else if (Op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) {
            if (uint64_t(Imm) * 8 > UINT64_MAX - Offset)
              return failure("stream offset overflows");
            Offset += uint64_t(Imm) * 8;
          }
          break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
          uint64_t Count, Skip;
          if (auto E = Number(Count))
            return E;
          if (auto E = Number(Skip))
            return E;
          if (auto E = Emit(Count, Skip))
            return E;
          break;
        }
        default:
          return failure("unsupported binding opcode");
        }
      } else {
        switch (Op) {
        case REBASE_OPCODE_SET_TYPE_IMM:
          Type = Imm;
          break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
          Segment = Imm;
          if (auto E = Number(Offset))
            return E;
          break;
        case REBASE_OPCODE_ADD_ADDR_ULEB: {
          auto N = uleb(P, End);
          if (!N)
            return N.takeError();
          Offset += *N;
          break;
        }
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
          if (uint64_t(Imm) * 8 > UINT64_MAX - Offset)
            return failure("stream offset overflows");
          Offset += uint64_t(Imm) * 8;
          break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
          if (auto E = Emit(Imm, 0))
            return E;
          break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
          uint64_t Count;
          if (auto E = Number(Count))
            return E;
          if (auto E = Emit(Count, 0))
            return E;
          break;
        }
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
          uint64_t Skip;
          if (auto E = Number(Skip))
            return E;
          if (auto E = Emit(1, Skip))
            return E;
          break;
        }
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
          uint64_t Count, Skip;
          if (auto E = Number(Count))
            return E;
          if (auto E = Number(Skip))
            return E;
          if (auto E = Emit(Count, Skip))
            return E;
          break;
        }
        default:
          return failure("unsupported rebase opcode");
        }
      }
    }
    return failure("unterminated fixup stream");
  }
  llvm::Error lazy(llvm::ArrayRef<uint8_t> Data) {
    const auto *P = Data.begin(), *End = Data.end();
    unsigned Segment = UINT32_MAX;
    uint64_t Offset = 0;
    int32_t Ordinal = 0;
    std::string Name;
    bool HasOrdinal = false, HasFields = false, Emitted = false;
    while (P < End) {
      if (auto E = check())
        return E;
      const uint8_t Byte = *P++, Op = Byte & 0xf0, Imm = Byte & 0xf;
      if (!Op) {
        if (Byte)
          return failure("invalid lazy bind terminator");
        if (!Emitted) {
          if (HasFields || llvm::any_of(llvm::ArrayRef(P, End),
                                        [](uint8_t B) { return B != 0; }))
            return failure("incomplete lazy bind program");
          return llvm::Error::success();
        }
        Segment = UINT32_MAX;
        Offset = 0;
        Ordinal = 0;
        Name.clear();
        HasOrdinal = HasFields = Emitted = false;
        continue;
      }
      if (Emitted)
        return failure("lazy bind must be followed immediately by DONE");
      HasFields = true;
      switch (Op) {
      case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        Ordinal = Imm;
        HasOrdinal = true;
        break;
      case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: {
        if (Imm)
          return failure("invalid lazy ordinal opcode");
        auto N = uleb(P, End);
        if (!N)
          return N.takeError();
        if (*N > INT32_MAX)
          return failure("dependency ordinal overflows");
        Ordinal = *N;
        HasOrdinal = true;
        break;
      }
      case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
        if (Imm)
          return failure("unsupported lazy binding symbol flags");
        auto S = string(llvm::ArrayRef(P, End), 0);
        if (!S)
          return S.takeError();
        P += S->size() + 1;
        Name = std::move(*S);
        break;
      }
      case BIND_OPCODE_SET_TYPE_IMM:
        if (Imm != BIND_TYPE_POINTER)
          return failure("unsupported lazy pointer type");
        break;
      case BIND_OPCODE_SET_ADDEND_SLEB: {
        if (Imm)
          return failure("invalid lazy addend opcode");
        unsigned Size = 0;
        const char *Error = nullptr;
        const auto Addend = llvm::decodeSLEB128(P, &Size, End, &Error);
        if (Error || !Size)
          return failure("invalid SLEB128");
        P += Size;
        if (Addend)
          return failure("nonzero lazy addends are unsupported");
        break;
      }
      case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: {
        Segment = Imm;
        auto N = uleb(P, End);
        if (!N)
          return N.takeError();
        Offset = *N;
        break;
      }
      case BIND_OPCODE_DO_BIND: {
        if (Imm || Segment == UINT32_MAX || !HasOrdinal || Name.empty())
          return failure(
              "lazy bind requires its own destination, ordinal and symbol");
        auto Address = slot(Segment, Offset);
        if (!Address)
          return Address.takeError();
        if (auto E = charge(2 * Name.size() + 64))
          return E;
        MachOProgramFixup F;
        F.Address = *Address;
        F.Symbol = Name;
        F.LibraryOrdinal = Ordinal;
        F.Bind = true;
        F.Origin = MachOProgramFixupOrigin::ClassicLazyBind;
        if (auto E = add(std::move(F)))
          return E;
        Emitted = true;
        break;
      }
      default:
        return failure("unsupported lazy binding opcode");
      }
    }
    if (HasFields || Emitted)
      return failure("unterminated lazy bind program");
    return llvm::Error::success();
  }
  llvm::Error chains(llvm::ArrayRef<uint8_t> Data);
  llvm::Error exports(llvm::ArrayRef<uint8_t> Data);
  llvm::Error read();
};

llvm::Error Reader::chains(llvm::ArrayRef<uint8_t> Data) {
  if (Data.size() < 28 || read32le(Data.data()) || read32le(Data.data() + 24))
    return failure("unsupported chained fixup header");
  const uint64_t Starts = read32le(Data.data() + 4),
                 Imports = read32le(Data.data() + 8),
                 Symbols = read32le(Data.data() + 12),
                 Count = read32le(Data.data() + 16);
  const uint32_t Format = read32le(Data.data() + 20);
  const uint64_t Width = Format == DYLD_CHAINED_IMPORT            ? 4
                         : Format == DYLD_CHAINED_IMPORT_ADDEND   ? 8
                         : Format == DYLD_CHAINED_IMPORT_ADDEND64 ? 16
                                                                  : 0;
  if (!Width || Starts < 28 || Starts > Imports || Imports > Symbols ||
      Symbols >= Data.size() || Count > RecordLimit ||
      Count > (Symbols - Imports) / Width || Imports - Starts < 4)
    return failure("invalid chained table layout");
  struct Import {
    std::string Name;
    int32_t Ordinal;
    int64_t Addend;
    bool Weak;
  };
  if (auto E = charge(Count * (2 * sizeof(Import) + 128)))
    return E;
  std::vector<Import> Table;
  Table.reserve(Count);
  for (uint64_t I = 0; I < Count; ++I) {
    const auto *P = Data.data() + Imports + I * Width;
    uint64_t Raw = Width == 16 ? read64le(P) : read32le(P);
    // The top fifteen encodings are special; ordinary ordinals retain their
    // unsigned bits even when the carrier's sign bit is set.
    const uint32_t Ordinal = Raw & (Width == 16 ? 0xffff : 0xff);
    if (Ordinal > (Width == 16 ? 0xfff0u : 0xf0u))
      return failure("special chained dependency ordinals are unsupported");
    const bool Weak = (Raw >> (Width == 16 ? 16 : 8)) & 1;
    const uint64_t NameOffset = Raw >> (Width == 16 ? 32 : 9);
    if (Width == 16 && ((Raw >> 17) & 0x7fff))
      return failure("reserved chained import bits");
    auto Name = string(Data.drop_front(Symbols), NameOffset);
    if (!Name)
      return Name.takeError();
    const int64_t Addend = Width == 16  ? int64_t(read64le(P + 8))
                           : Width == 8 ? int32_t(read32le(P + 4))
                                        : 0;
    if (!Undefined.contains({*Name, Ordinal, Weak}))
      return failure("chained import disagrees with undefined symbol metadata");
    Table.push_back({std::move(*Name), int32_t(Ordinal), Addend, Weak});
  }
  const uint32_t SegmentCount = read32le(Data.data() + Starts);
  if (SegmentCount != Segments.size() ||
      SegmentCount > (Imports - Starts - 4) / 4)
    return failure("invalid chained segment count");
  std::set<std::pair<uint64_t, uint64_t>> Extents;
  const uint64_t StartHeaderSize = 4 + uint64_t(SegmentCount) * 4;
  for (uint32_t I = 0; I < SegmentCount; ++I) {
    if (auto E = check())
      return E;
    const uint64_t Offset = read32le(Data.data() + Starts + 4 + I * 4);
    if (!Offset)
      continue;
    if (Offset < StartHeaderSize || Offset > Imports - Starts ||
        22 > Imports - Starts - Offset)
      return failure("invalid chained segment header");
    const auto *P = Data.data() + Starts + Offset;
    const uint64_t Size = read32le(P), PageSize = read16le(P + 4),
                   SegmentOffset = read64le(P + 8), Pages = read16le(P + 20);
    const auto PointerFormat = read16le(P + 6);
    const auto &S = Segments[I];
    if (Size < 22 || Size > Imports - Starts - Offset ||
        Pages > (Size - 22) / 2 || Size != 22 + Pages * 2 ||
        (PageSize != 4096 && PageSize != 16384) ||
        (PointerFormat != DYLD_CHAINED_PTR_64 &&
         PointerFormat != DYLD_CHAINED_PTR_64_OFFSET) ||
        read32le(P + 16) || S.vmaddr < HeaderAddress ||
        SegmentOffset != S.vmaddr - HeaderAddress ||
        Pages > (S.vmsize / PageSize + bool(S.vmsize % PageSize)))
      return failure("unsupported or malformed chained segment");
    for (const auto &[Begin, End] : Extents)
      if (Offset < End && Offset + Size > Begin)
        return failure("overlapping chained segment headers");
    if (auto E = charge(128))
      return E;
    Extents.insert({Offset, Offset + Size});
    for (uint64_t Page = 0; Page < Pages; ++Page) {
      uint64_t At = read16le(P + 22 + Page * 2);
      if (At == DYLD_CHAINED_PTR_START_NONE)
        continue;
      if (At & DYLD_CHAINED_PTR_START_MULTI)
        return failure("multi-start chains are unsupported");
      while (true) {
        if (At > PageSize || 8 > PageSize - At)
          return failure("chain escapes its page");
        const uint64_t OffsetInSegment = Page * PageSize + At;
        auto Address = slot(I, OffsetInSegment);
        if (!Address)
          return Address.takeError();
        const uint64_t Raw =
            read64le(Bytes.data() + S.fileoff + OffsetInSegment);
        MachOProgramFixup F;
        F.Address = *Address;
        F.Bind = Raw >> 63;
        F.Origin = MachOProgramFixupOrigin::Chained;
        if (F.Bind) {
          const uint64_t Index = Raw & 0xffffff;
          if (Index >= Table.size() || ((Raw >> 32) & 0x7ffff))
            return failure("invalid chained pointer import");
          const auto &Import = Table[Index];
          if (auto E = charge(2 * Import.Name.size() + 64))
            return E;
          F.Symbol = Import.Name;
          F.LibraryOrdinal = Import.Ordinal;
          F.WeakImport = Import.Weak;
          // Generic64 pointer addends are unsigned eight-bit values.
          if (llvm::AddOverflow(Import.Addend, int64_t((Raw >> 24) & 0xff),
                                F.Addend))
            return failure("chained pointer addend overflows");
        } else {
          if ((Raw >> 44) & 0x7f)
            return failure("reserved chained rebase bits");
          F.Target = (Raw & 0xfffffffffULL) | (((Raw >> 36) & 0xff) << 56);
          if (PointerFormat == DYLD_CHAINED_PTR_64_OFFSET) {
            if (F.Target > UINT64_MAX - HeaderAddress)
              return failure("chained rebase target overflows");
            F.Target += HeaderAddress;
          }
        }
        if (auto E = add(std::move(F)))
          return E;
        const uint64_t Next = (Raw >> 51) & 0xfff;
        if (!Next)
          break;
        At += Next * 4;
      }
    }
  }
  return llvm::Error::success();
}

llvm::Error Reader::exports(llvm::ArrayRef<uint8_t> Data) {
  struct Pending {
    uint64_t Offset;
    std::string Name;
  };
  std::vector<Pending> Work{{0, {}}};
  std::map<uint64_t, uint64_t> Nodes;
  std::set<std::string> Names;
  while (!Work.empty()) {
    if (auto E = charge(128))
      return E;
    auto Node = std::move(Work.back());
    Work.pop_back();
    if (Nodes.size() >= RecordLimit || Node.Offset >= Data.size() ||
        Nodes.contains(Node.Offset))
      return failure("invalid, repeated or excessive export trie node");
    const auto *P = Data.data() + Node.Offset, *End = Data.end();
    auto Size = uleb(P, End);
    if (!Size)
      return Size.takeError();
    if (*Size > uint64_t(End - P))
      return failure("export terminal escapes the trie");
    const auto *TerminalEnd = P + *Size;
    if (*Size) {
      auto Flags = uleb(P, TerminalEnd);
      if (!Flags)
        return Flags.takeError();
      if (*Flags != EXPORT_SYMBOL_FLAGS_KIND_REGULAR &&
          *Flags != EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL)
        return failure("unsupported export kind, weak export or resolver");
      auto Offset = uleb(P, TerminalEnd);
      if (!Offset)
        return Offset.takeError();
      if (P != TerminalEnd || Node.Name.empty() ||
          *Offset > UINT64_MAX - HeaderAddress)
        return failure("invalid export terminal");
      const uint64_t Address = HeaderAddress + *Offset;
      const bool Mapped = llvm::any_of(Segments, [&](const auto &S) {
        return S.initprot && Address >= S.vmaddr &&
               Address - S.vmaddr < S.vmsize;
      });
      if (!Mapped || !Names.insert(Node.Name).second)
        return failure("unmapped or duplicate exported symbol");
      if (auto E = charge(2 * Node.Name.size() +
                          2 * sizeof(MachOProgramExport) + 128))
        return E;
      Out.Exports.push_back({Node.Name, Address,
                             *Flags == EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL});
    }
    P = TerminalEnd;
    if (P == End)
      return failure("export node has no child count");
    const unsigned Count = *P++;
    std::set<uint8_t> Prefixes;
    for (unsigned I = 0; I < Count; ++I) {
      auto Edge = string(llvm::ArrayRef(P, End), 0);
      if (!Edge)
        return Edge.takeError();
      if (!Prefixes.insert(uint8_t(Edge->front())).second ||
          Node.Name.size() >
              ::neverd::limits::loader::
                  kMachOProgramLinkingReaderExportsNodeNameSizeLimit ||
          Edge->size() > ::neverd::limits::loader::kMaxMachOExportNameBytes -
                             Node.Name.size())
        return failure("ambiguous or excessive export name");
      P += Edge->size() + 1;
      auto Offset = uleb(P, End);
      if (!Offset)
        return Offset.takeError();
      if (Work.size() >= RecordLimit || *Offset >= Data.size())
        return failure("invalid or excessive export child");
      if (auto E = charge(2 * (Node.Name.size() + Edge->size()) + 128))
        return E;
      Work.push_back({*Offset, Node.Name + *Edge});
    }
    const uint64_t NodeEnd = P - Data.data();
    auto Next = Nodes.lower_bound(Node.Offset);
    if ((Next != Nodes.end() && Next->first < NodeEnd) ||
        (Next != Nodes.begin() && std::prev(Next)->second > Node.Offset))
      return failure("overlapping export trie nodes");
    Nodes.emplace(Node.Offset, NodeEnd);
    ++Out.Records;
  }
  return llvm::Error::success();
}

llvm::Error Reader::read() {
  if (auto E = check())
    return E;
  auto Parsed =
      llvm::object::ObjectFile::createMachOObjectFile(llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                          Bytes.size()),
          "execution linking"));
  if (!Parsed)
    return Parsed.takeError();
  const auto &Obj = **Parsed;
  if (!Obj.is64Bit() || !Obj.isLittleEndian())
    return failure("requires thin little-endian Mach-O64");
  std::optional<dyld_info_command> Classic;
  std::optional<linkedit_data_command> Chained, Exports;
  std::optional<symtab_command> Symbols;
  std::optional<dysymtab_command> Dynamic;
  unsigned HeaderSegments = 0;
  for (const auto &LC : Obj.load_commands()) {
    if (auto E = check())
      return E;
    switch (LC.C.cmd) {
    case LC_SEGMENT_64: {
      const auto S = Obj.getSegment64LoadCommand(LC);
      if (S.vmaddr > UINT64_MAX - S.vmsize || S.filesize > S.vmsize ||
          S.fileoff > Bytes.size() || S.filesize > Bytes.size() - S.fileoff)
        return failure("invalid segment extent");
      if (auto E = charge(2 * sizeof(S)))
        return E;
      Segments.push_back(S);
      if (!S.fileoff && S.filesize) {
        HeaderAddress = S.vmaddr;
        ++HeaderSegments;
      }
      for (uint32_t I = 0; I < S.nsects; ++I)
        if (Obj.getSection64(LC, I).nreloc)
          return failure("section relocations are unsupported");
      break;
    }
    case LC_ID_DYLIB:
    case LC_LOAD_DYLIB:
    case LC_LOAD_WEAK_DYLIB:
    case LC_REEXPORT_DYLIB:
    case LC_LOAD_UPWARD_DYLIB:
    case LC_LAZY_LOAD_DYLIB: {
      const auto D = Obj.getDylibIDLoadCommand(LC);
      if (D.dylib.name < sizeof(dylib_command))
        return failure("invalid dependency string offset");
      auto Name =
          string(llvm::ArrayRef(reinterpret_cast<const uint8_t *>(LC.Ptr),
                                LC.C.cmdsize),
                 D.dylib.name);
      if (!Name)
        return Name.takeError();
      if (LC.C.cmd == LC_ID_DYLIB) {
        if (Out.Identity)
          return failure("duplicate dylib identity");
        Out.Identity = MachOProgramDependency{LC.C.cmd, std::move(*Name),
                                              D.dylib.current_version,
                                              D.dylib.compatibility_version};
        break;
      }
      if (Out.Dependencies.size() >= RecordLimit)
        return failure("too many dependencies");
      if (auto E = charge(2 * sizeof(MachOProgramDependency)))
        return E;
      Out.Dependencies.push_back({LC.C.cmd, std::move(*Name),
                                  D.dylib.current_version,
                                  D.dylib.compatibility_version});
      break;
    }
    case LC_DYLD_INFO:
    case LC_DYLD_INFO_ONLY:
      if (Classic)
        return failure("duplicate classic fixup command");
      Classic = Obj.getDyldInfoLoadCommand(LC);
      break;
    case LC_DYLD_CHAINED_FIXUPS:
      if (Chained)
        return failure("duplicate chained fixup command");
      Chained = Obj.getLinkeditDataLoadCommand(LC);
      break;
    case LC_DYLD_EXPORTS_TRIE:
      if (Exports)
        return failure("duplicate export trie command");
      Exports = Obj.getLinkeditDataLoadCommand(LC);
      break;
    case LC_SYMTAB:
      if (Symbols)
        return failure("duplicate symbol table");
      Symbols = Obj.getSymtabLoadCommand();
      break;
    case LC_DYSYMTAB:
      if (Dynamic)
        return failure("duplicate dynamic symbol table");
      Dynamic = Obj.getDysymtabLoadCommand();
      if (Dynamic->nextrel || Dynamic->nlocrel)
        return failure("external/local relocations are unsupported");
      break;
    default:
      break;
    }
  }
  if (HeaderSegments != 1)
    return failure("requires one mapped Mach header");
  if (Classic && Chained)
    return failure("mixed classic and chained fixup sources");
  if (Symbols) {
    if (Symbols->nsyms > RecordLimit)
      return failure("too many symbol records");
    auto Entries = bytes(Symbols->symoff, uint64_t(Symbols->nsyms) * 16);
    if (!Entries)
      return Entries.takeError();
    for (uint32_t I = 0; I < Symbols->nsyms; ++I) {
      if (auto E = check())
        return E;
      const auto *P = Entries->data() + uint64_t(I) * 16;
      if ((P[4] & N_STAB) || (P[4] & N_TYPE) != N_UNDF)
        continue;
      if (!Dynamic || I < Dynamic->iundefsym ||
          I - Dynamic->iundefsym >= Dynamic->nundefsym)
        return failure("undefined symbol is outside the declared range");
    }
  }
  if (Dynamic && Dynamic->nundefsym) {
    if (!Symbols || Dynamic->nundefsym > RecordLimit ||
        Dynamic->iundefsym > Symbols->nsyms ||
        Dynamic->nundefsym > Symbols->nsyms - Dynamic->iundefsym)
      return failure("invalid undefined symbol range");
    auto Names = bytes(Symbols->stroff, Symbols->strsize);
    if (!Names)
      return Names.takeError();
    auto Entries = bytes(Symbols->symoff, uint64_t(Symbols->nsyms) * 16);
    if (!Entries)
      return Entries.takeError();
    for (uint32_t I = 0; I < Dynamic->nundefsym; ++I) {
      const auto *P = Entries->data() + uint64_t(Dynamic->iundefsym + I) * 16;
      const uint16_t Description = read16le(P + 6);
      const uint32_t Ordinal = Description >> 8;
      if (P[4] != (uint32_t(N_EXT) | uint32_t(N_UNDF)) || P[5] ||
          read64le(P + 8) ||
          (Description & ~(0xff00u | N_WEAK_REF | REFERENCE_TYPE)) ||
          (Description & REFERENCE_TYPE) > REFERENCE_FLAG_UNDEFINED_LAZY)
        return failure("unsupported undefined symbol kind");
      if (!Ordinal || Ordinal >= DYNAMIC_LOOKUP_ORDINAL)
        return failure("undefined symbol requires an ordinary library ordinal");
      auto Name = string(*Names, read32le(P));
      if (!Name)
        return Name.takeError();
      if (auto E = charge(2 * sizeof(Identity) + 128))
        return E;
      if (!Undefined
               .emplace(std::move(*Name), Ordinal,
                        bool(Description & N_WEAK_REF))
               .second)
        return failure("duplicate undefined symbol identity");
    }
  }
  if (Classic) {
    if (Classic->weak_bind_size)
      return failure("weak binding streams are unsupported");
    for (const auto &[Offset, Size, Bind] :
         {std::tuple{Classic->rebase_off, Classic->rebase_size, false},
          std::tuple{Classic->bind_off, Classic->bind_size, true}}) {
      if (!Size)
        continue;
      auto Data = bytes(Offset, Size);
      if (!Data)
        return Data.takeError();
      if (auto E = stream(*Data, Bind))
        return E;
    }
    if (Classic->lazy_bind_size) {
      auto Data = bytes(Classic->lazy_bind_off, Classic->lazy_bind_size);
      if (!Data)
        return Data.takeError();
      if (auto E = lazy(*Data))
        return E;
    }
  }
  if (Chained && Chained->datasize) {
    auto Data = bytes(Chained->dataoff, Chained->datasize);
    if (!Data)
      return Data.takeError();
    if (auto E = chains(*Data))
      return E;
  }
  if (Exports && Classic && Classic->export_size)
    return failure("multiple export trie sources");
  const uint64_t ExportSize = Exports   ? Exports->datasize
                              : Classic ? Classic->export_size
                                        : 0;
  const uint64_t ExportOffset = Exports   ? Exports->dataoff
                                : Classic ? Classic->export_off
                                          : 0;
  if (ExportSize) {
    auto Data = bytes(ExportOffset, ExportSize);
    if (!Data)
      return Data.takeError();
    if (auto E = exports(*Data))
      return E;
  }
  if (Undefined != Bound)
    return failure("undefined symbols do not have a complete binding closure");
  Out.Records += RawFixups + Segments.size() + Out.Dependencies.size() +
                 (Symbols ? Symbols->nsyms : 0);
  return check();
}
} // namespace

llvm::Expected<MachOProgramLinking>
readMachOProgramLinking(llvm::ArrayRef<uint8_t> Bytes,
                        uint64_t MetadataByteLimit,
                        llvm::function_ref<llvm::Error()> CheckPreparation) {
  Reader R(Bytes, MetadataByteLimit, CheckPreparation);
  if (auto E = R.read())
    return std::move(E);
  return std::move(R.Out);
}
} // namespace neverd
