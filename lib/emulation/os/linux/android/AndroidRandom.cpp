//===- AndroidRandom.cpp - Process-local Bionic pseudo-random state ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently implements the Android 9 default PRNG arithmetic and guest
/// seed access. Versioned behavioral references: android-native-emulation.md.

#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
uint32_t Bionic::nextRandom() {
  const unsigned Front = (RandomRear + 3) % 31;
  RandomWords[Front] += RandomWords[RandomRear];
  const uint32_t Result = RandomWords[Front] >> 1;
  RandomRear = (RandomRear + 1) % 31;
  return Result;
}

void Bionic::seedRandom(uint32_t Seed) {
  RandomWords[0] = Seed;
  for (unsigned I = 1; I != 31; ++I) {
    // The seed is an unsigned int, but the recurrence reads each word as a
    // signed int. Keep intermediate arithmetic wide and truncation explicit.
    const int64_t Previous = int64_t(RandomWords[I - 1]) -
                             ((RandomWords[I - 1] >> 31) ? 0x100000000LL : 0);
    int64_t Next = 16807 * (Previous % 127773) - 2836 * (Previous / 127773);
    if (Next <= 0)
      Next += 0x7fffffff;
    RandomWords[I] = static_cast<uint32_t>(Next);
  }
  RandomRear = 0;
  for (unsigned I = 0; I != 310; ++I)
    nextRandom();
  RandomInitialized = true;
}

BionicResult Bionic::randomCall(const NativeCallEvent &Call) {
  if (Call.Name == symbol::RandR) {
    const uint64_t Address = Call.Arguments[0];
    if (auto E = access(Address, 4, Read | Write))
      return std::move(E);
    uint8_t Bytes[4];
    if (auto E = CPU.read(Address, Bytes))
      return std::move(E);
    const uint32_t Next =
        llvm::support::endian::read32le(Bytes) * 1103515245u + 12345u;
    llvm::support::endian::write32le(Bytes, Next);
    if (auto E = CPU.write(Address, Bytes))
      return std::move(E);
    return value(Next & 0x7fffffff);
  }
  if (Call.Name == symbol::Srand || Call.Name == symbol::Srandom) {
    seedRandom(static_cast<uint32_t>(Call.Arguments[0]));
    return value(0); // Both entry points have a void result.
  }
  assert(Call.Name == symbol::Rand || Call.Name == symbol::Random);
  if (!RandomInitialized)
    seedRandom(1);
  return value(nextRandom());
}
} // namespace neverd::emulation::android_model
