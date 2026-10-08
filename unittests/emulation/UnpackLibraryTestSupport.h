//===- UnpackLibraryTestSupport.h - Independent packed DLL -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_UNPACK_LIBRARY_TEST_SUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_UNPACK_LIBRARY_TEST_SUPPORT_H
#include "UnpackTestSupport.h"

#include "llvm/Support/Endian.h"

namespace neverd::unpack::test::library {
#define NEVERD_LIBRARY_VALUE(Name, Value)                                      \
  inline constexpr uint64_t Name = Value;
#define NEVERD_LIBRARY_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "fixtures/UnpackLibraryCases.def"
#undef NEVERD_LIBRARY_TEXT
#undef NEVERD_LIBRARY_VALUE

/// Encode the independently linked code, then make the ordinary loader or TLS
/// callback responsible for restoring it. No production packing logic is used.
inline std::vector<uint8_t> pack(const Image &Original, uint32_t Mode) {
  using namespace llvm::support::endian;
  auto Bytes = Original.File;
  const auto *Body = Original.section(BodySection);
  const auto *Record = Original.section(RecordSection);
  if (!Body || !Record || Original.Entry != Body->RVA ||
      Body->VirtualSize > Capacity || Body->VirtualSize > Body->FileSize ||
      Record->FileSize < RecordHeader + Capacity ||
      !Original.Exports.contains("loader")) {
    ADD_FAILURE() << "invalid independent DLL fixture";
    return {};
  }
  uint8_t *Header = Bytes.data() + Record->FileOffset;
  write32le(Header, Mode);
  write32le(Header + 4, Key);
  write32le(Header + 8, Body->VirtualSize);
  for (uint32_t I = 0; I < Body->VirtualSize; ++I) {
    Header[RecordHeader + I] = Bytes[Body->FileOffset + I] ^ Key;
    Bytes[Body->FileOffset + I] = 0;
  }
  if (Mode != TLSMode)
    write32le(Bytes.data() + Original.EntryOffset,
              Original.Exports.at("loader"));
  if (Mode == NoTLSCallbacksMode || Mode == EmptyTLSCallbacksMode) {
    auto FileBytes = [&](uint64_t RVA,
                         uint64_t Size = sizeof(uint64_t)) -> uint8_t * {
      for (const auto &Section : Original.Sections)
        if (RVA >= Section.RVA && RVA - Section.RVA <= Section.FileSize &&
            Size <= Section.FileSize - (RVA - Section.RVA))
          return Bytes.data() + Section.FileOffset + RVA - Section.RVA;
      return nullptr;
    };
    const auto TLS = Original.directory(llvm::COFF::TLS_TABLE);
    uint64_t Cleared =
        uint64_t(TLS.RelativeVirtualAddress) +
        offsetof(llvm::object::coff_tls_directory64, AddressOfCallBacks);
    uint8_t *Callbacks = FileBytes(Cleared);
    if (Callbacks && Mode == EmptyTLSCallbacksMode) {
      const uint64_t Address = read64le(Callbacks);
      Cleared = Address - Original.Base;
      Callbacks = Address >= Original.Base ? FileBytes(Cleared) : nullptr;
    }
    if (!Callbacks) {
      ADD_FAILURE() << "invalid independent DLL TLS fixture";
      return {};
    }
    write64le(Callbacks, 0);
    // A null pointer has no base relocation. Retire its original fixup so
    // the independent native loader can also rebase the protected fixture.
    const auto Relocations =
        Original.directory(llvm::COFF::BASE_RELOCATION_TABLE);
    uint64_t At = Relocations.RelativeVirtualAddress;
    const uint64_t End = At + Relocations.Size;
    while (At < End) {
      uint8_t *Block = FileBytes(At);
      if (!Block || End - At < 8) {
        ADD_FAILURE() << "invalid independent DLL relocation block";
        return {};
      }
      const uint64_t Page = read32le(Block), Size = read32le(Block + 4);
      if (Size < 8 || Size % 2 || Size > End - At || !FileBytes(At, Size)) {
        ADD_FAILURE() << "invalid independent DLL relocation extent";
        return {};
      }
      for (uint64_t I = 8; I < Size; I += 2) {
        const uint16_t Entry = read16le(Block + I);
        if (Entry >> 12 == llvm::COFF::IMAGE_REL_BASED_DIR64 &&
            Page + (Entry & 0xfff) == Cleared)
          write16le(Block + I, 0);
      }
      At += Size;
    }
  }
  return Bytes;
}

inline bool write(const std::filesystem::path &Path,
                  llvm::ArrayRef<uint8_t> Bytes) {
  std::ofstream Stream(Path, std::ios::binary);
  return bool(Stream.write(reinterpret_cast<const char *>(Bytes.data()),
                           std::streamsize(Bytes.size())));
}
} // namespace neverd::unpack::test::library
#endif
