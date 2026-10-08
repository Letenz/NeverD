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
