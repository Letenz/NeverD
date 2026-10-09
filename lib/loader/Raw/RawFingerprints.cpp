//===- RawFingerprints.cpp - Structures that name a binary file's code ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Readers of the structures RawFingerprints.def lists.  Each reads the start
// of a binary file and names the processor its code runs on and where.
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Raw/ISAIdentify.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>

namespace neverd {
namespace {

#define NEVERD_CORTEX_M_LIMIT(Id, Value)                                       \
  constexpr uint64_t kCortexM##Id = Value;
#include "neverd/loader/Raw/RawFingerprints.def"

struct StackRange {
  uint64_t Low, High;
};

constexpr StackRange CortexMStacks[] = {
#define NEVERD_CORTEX_M_STACK(Low, High) {Low, High},
#include "neverd/loader/Raw/RawFingerprints.def"
};

/// An ARMv6-M/ARMv7-M vector table: the initial stack pointer in RAM, then
/// Thumb handler addresses -- Reset and HardFault always -- with the four
/// reserved words zero and every handler near the others.  The table opens
/// the image, so the image starts where the handlers' addresses, rounded
/// down to the image's size, do.
std::optional<RawFingerprint>
readCortexMVectorTable(llvm::ArrayRef<uint8_t> Bytes, llvm::StringRef Name,
                       llvm::StringRef Processor) {
  constexpr size_t Words = kCortexMWords;
  constexpr size_t Reset = 1, HardFault = 3;
  if (Bytes.size() < Words * 4)
    return std::nullopt;
  std::array<uint32_t, Words> Word;
  for (size_t I = 0; I < Words; ++I)
    Word[I] = llvm::support::endian::read32le(Bytes.data() + I * 4);
  const uint32_t Stack = Word[0];
  if (Stack % 4 != 0 || llvm::none_of(CortexMStacks, [&](StackRange R) {
        return Stack >= R.Low && Stack < R.High;
      }))
    return std::nullopt;
  if (!(Word[Reset] & 1) || !(Word[HardFault] & 1))
    return std::nullopt;
  uint32_t Low = Word[Reset], High = Word[Reset];
  for (size_t I = 1; I < Words; ++I) {
    const bool Reserved = I >= kCortexMFirstReserved &&
                          I < kCortexMFirstReserved + kCortexMReserved;
    if (Reserved ? Word[I] != 0 : Word[I] != 0 && !(Word[I] & 1))
      return std::nullopt;
    if (Word[I] != 0) {
      Low = std::min(Low, Word[I]);
      High = std::max(High, Word[I]);
    }
  }
  if (High - Low > kCortexMHandlerSpread)
    return std::nullopt;
  RawFingerprint Found;
  Found.Name = Name;
  Found.Processor = Processor;
  Found.Entry = Word[Reset] & ~uint32_t(1);
  Found.Base = Found.Entry & ~(llvm::PowerOf2Ceil(Bytes.size()) - 1);
  return Found;
}

} // namespace

std::optional<RawFingerprint>
readRawFingerprint(llvm::ArrayRef<uint8_t> Bytes) {
#define NEVERD_RAW_FINGERPRINT(Id, Name, Processor)                            \
  if (auto Found = read##Id(Bytes, Name, Processor))                           \
    return Found;
#include "neverd/loader/Raw/RawFingerprints.def"
  return std::nullopt;
}

} // namespace neverd
