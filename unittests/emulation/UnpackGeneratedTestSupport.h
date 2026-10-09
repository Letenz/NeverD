//===- UnpackGeneratedTestSupport.h - Independent packed fixtures -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_UNPACKGENERATEDTESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_UNPACKGENERATEDTESTSUPPORT_H

#include "UnpackTestSupport.h"

#include "llvm/Support/Endian.h"

namespace neverd::unpack::test::generated {
#define NEVERD_GENERATED_VALUE(Name, Value)                                    \
  inline constexpr uint64_t Name = Value;
#define NEVERD_GENERATED_MODE(Name, Value)                                     \
  inline constexpr uint32_t Name##Mode = Value;
#define NEVERD_GENERATED_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "fixtures/UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_TEXT
#undef NEVERD_GENERATED_MODE
#undef NEVERD_GENERATED_VALUE

/// Move independently linked code into the fixture's encoded pack record.
/// Call-site mutations, when requested, are already present in Bytes.
inline std::vector<uint8_t> pack(const Image &Original,
                                 std::vector<uint8_t> Bytes, uint32_t Mode) {
  using namespace llvm::support::endian;
  const auto *Pay = Original.section(PackSection);
  const auto Loader = Original.Exports.find(LoaderExport);
  if (!Pay || Loader == Original.Exports.end() ||
      Pay->FileSize < RelayOffset + Capacity ||
      Bytes.size() != Original.File.size()) {
    ADD_FAILURE() << MissingRecord;
    return {};
  }
  uint8_t *Record = Bytes.data() + Pay->FileOffset;
  write32le(Record + ModeOffset, Mode);
  write32le(Record + KeyOffset, uint32_t(Key));
  auto Move = [&](const char *Name, uint64_t SizeAt, uint64_t DataAt) {
    const auto *S = Original.section(Name);
    if (!S || S->VirtualSize > Capacity || S->VirtualSize > S->FileSize) {
      ADD_FAILURE() << Name << RecordOverflow;
      return false;
    }
    for (uint32_t I = 0; I < S->VirtualSize; ++I)
      Record[DataAt + I] = uint8_t(Bytes[S->FileOffset + I] ^ Key);
    std::fill_n(Bytes.begin() + S->FileOffset, S->FileSize, 0);
    write32le(Record + SizeAt, S->VirtualSize);
    return true;
  };
  if (!Move(ProgramSection, ProgramBytesOffset, ProgramOffset) ||
      (Mode == StagedMode &&
       !Move(RelaySection, RelayBytesOffset, RelayOffset)))
    return {};
  write32le(Bytes.data() + Original.EntryOffset, Loader->second);
  return Bytes;
}
} // namespace neverd::unpack::test::generated
#endif
