//===- InputDigest.cpp - SHA-256 of a loaded input file -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/InputDigest.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/SHA256.h"
#include "llvm/TargetParser/Host.h"

#include <climits>
#include <cstring>
#include <iterator>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#define NEVERD_SHA_EXTENSIONS 1
#include <immintrin.h>
#endif

namespace neverd {

#ifdef NEVERD_SHA_EXTENSIONS
namespace {

#define NEVERD_SHA256_SIZE(Name, Value) constexpr size_t Name = Value;
#define NEVERD_SHA256_BYTE(Name, Value) constexpr uint8_t Name = Value;
#define NEVERD_SHA256_LANES(Name, Value) constexpr int Name = Value;
#define NEVERD_SHA256_TARGET_FEATURES(Features)                                \
  constexpr llvm::StringLiteral TargetFeatures(Features);
#include "SHA256.def"

constexpr uint32_t InitialState[] = {
#define NEVERD_SHA256_INITIAL_STATE(Value) Value,
#include "SHA256.def"
};

constexpr uint32_t RoundConstants[] = {
#define NEVERD_SHA256_ROUND_CONSTANT(Value) Value,
#include "SHA256.def"
};

constexpr size_t WordBytes = sizeof(uint32_t);
constexpr size_t LengthBytes = sizeof(uint64_t);
constexpr size_t RegisterBytes = sizeof(__m128i);
constexpr size_t HalfRegisterBytes = RegisterBytes / 2;
constexpr size_t WordsPerRegister = RegisterBytes / WordBytes;
constexpr size_t StateWords = std::size(InitialState);

/// The rounds take the message schedule, and the round constants, one
/// register -- a quad of words -- at a time; a block's own words are the
/// first quads of its schedule.
constexpr size_t Quads = std::size(RoundConstants) / WordsPerRegister;
constexpr size_t BlockQuads = BlockBytes / RegisterBytes;

/// The pshufb control that reverses the bytes of each word of a register:
/// the message words are big-endian.
constexpr std::array<uint8_t, RegisterBytes> wordByteSwap() {
  std::array<uint8_t, RegisterBytes> Control = {};
  for (size_t I = 0; I != RegisterBytes; ++I)
    Control[I] =
        static_cast<uint8_t>(I - I % WordBytes + WordBytes - 1 - I % WordBytes);
  return Control;
}
constexpr std::array<uint8_t, RegisterBytes> WordByteSwap = wordByteSwap();

/// Whether the processor has every feature compress() is compiled for.
bool hasTargetFeatures() {
  const llvm::StringMap<bool> Host = llvm::sys::getHostCPUFeatures();
  llvm::SmallVector<llvm::StringRef, 4> Features;
  TargetFeatures.split(Features, ',');
  return llvm::all_of(Features,
                      [&](llvm::StringRef Name) { return Host.lookup(Name); });
}

/// Run \p Blocks blocks of \p Data through the compression function.
///
/// The instructions keep the state as ABEF and CDGH and take the message
/// schedule, with the round constants added, a quad at a time.  The schedule
/// is kept in as many registers as a block has quads: each quad past the
/// block's own is derived from the ones before it, into the register of the
/// one it no longer needs.
///
/// The function is compiled for the target features SHA256.def names, which
/// the build need not enable; sha256() calls it only on a processor that has
/// them.
#if defined(_MSC_VER) && !defined(__clang__)
#define NEVERD_SHA256_TARGET_FEATURES(Features)
#else
#define NEVERD_SHA256_TARGET_FEATURES(Features)                                \
  __attribute__((target(Features)))
#endif
#include "SHA256.def"
void compress(uint32_t *State, const uint8_t *Data, size_t Blocks) {
  const __m128i ByteSwap =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(WordByteSwap.data()));
  __m128i Swapped = _mm_shuffle_epi32(
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(State)),
      SwapWordPairs); // CDAB
  __m128i CDGH =
      _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(
                            State + WordsPerRegister)),
                        ReverseWords);                              // EFGH
  __m128i ABEF = _mm_alignr_epi8(Swapped, CDGH, HalfRegisterBytes); // ABEF
  CDGH = _mm_blend_epi16(CDGH, Swapped, UpperHalf);                 // CDGH

  for (; Blocks != 0; --Blocks, Data += BlockBytes) {
    const __m128i SavedABEF = ABEF, SavedCDGH = CDGH;
    __m128i Schedule[BlockQuads];
    for (size_t Quad = 0; Quad != BlockQuads; ++Quad)
      Schedule[Quad] =
          _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(
                               Data + Quad * RegisterBytes)),
                           ByteSwap);
    for (size_t Quad = 0; Quad != Quads; ++Quad) {
      const __m128i Current = Schedule[Quad % BlockQuads];
      __m128i Message = _mm_add_epi32(
          Current, _mm_loadu_si128(reinterpret_cast<const __m128i *>(
                       RoundConstants + Quad * WordsPerRegister)));
      CDGH = _mm_sha256rnds2_epu32(CDGH, ABEF, Message);
      // Finish the next quad: add W[t-7], from the quad before this one and
      // this one, and sigma1 of W[t-2], from this one.
      if (const size_t Next = Quad + 1; Next >= BlockQuads && Next < Quads) {
        __m128i &Words = Schedule[Next % BlockQuads];
        const __m128i Previous = Schedule[(Quad + BlockQuads - 1) % BlockQuads];
        Words =
            _mm_add_epi32(Words, _mm_alignr_epi8(Current, Previous, WordBytes));
        Words = _mm_sha256msg2_epu32(Words, Current);
      }
      Message = _mm_shuffle_epi32(Message, UpperWordsLow);
      ABEF = _mm_sha256rnds2_epu32(ABEF, CDGH, Message);
      // Start the quad whose W[t-16] is the quad before this one, in whose
      // register it goes: add sigma0 of W[t-15], from that quad and this one.
      if (const size_t Later = Quad + BlockQuads - 1;
          Later >= BlockQuads && Later < Quads) {
        __m128i &Words = Schedule[Later % BlockQuads];
        Words = _mm_sha256msg1_epu32(Words, Current);
      }
    }
    ABEF = _mm_add_epi32(ABEF, SavedABEF);
    CDGH = _mm_add_epi32(CDGH, SavedCDGH);
  }

  Swapped = _mm_shuffle_epi32(ABEF, ReverseWords);          // FEBA
  CDGH = _mm_shuffle_epi32(CDGH, SwapWordPairs);            // DCHG
  ABEF = _mm_blend_epi16(Swapped, CDGH, UpperHalf);         // DCBA
  CDGH = _mm_alignr_epi8(CDGH, Swapped, HalfRegisterBytes); // HGFE
  _mm_storeu_si128(reinterpret_cast<__m128i *>(State), ABEF);
  _mm_storeu_si128(reinterpret_cast<__m128i *>(State + WordsPerRegister), CDGH);
}

/// How many blocks the padding fills when \p Rest bytes of the message are
/// left after its whole blocks: the byte that ends the message and its
/// length follow them.
constexpr size_t tailBlocks(size_t Rest) {
  return llvm::divideCeil(Rest + sizeof(MessageEnd) + LengthBytes, BlockBytes);
}

std::array<uint8_t, 32> sha256WithExtensions(llvm::ArrayRef<uint8_t> Data) {
  uint32_t State[StateWords];
  llvm::copy(InitialState, State);
  const size_t Blocks = Data.size() / BlockBytes;
  compress(State, Data.data(), Blocks);

  uint8_t Tail[tailBlocks(BlockBytes - 1) * BlockBytes] = {};
  const size_t Rest = Data.size() % BlockBytes;
  if (Rest != 0)
    std::memcpy(Tail, Data.data() + Blocks * BlockBytes, Rest);
  Tail[Rest] = MessageEnd;
  const size_t TailBytes = tailBlocks(Rest) * BlockBytes;
  llvm::support::endian::write64be(Tail + TailBytes - LengthBytes,
                                   static_cast<uint64_t>(Data.size()) *
                                       CHAR_BIT);
  compress(State, Tail, TailBytes / BlockBytes);

  std::array<uint8_t, 32> Digest;
  static_assert(sizeof(Digest) == sizeof(State),
                "the digest is the final state, big-endian");
  for (size_t I = 0; I != StateWords; ++I)
    llvm::support::endian::write32be(Digest.data() + I * WordBytes, State[I]);
  return Digest;
}

} // namespace
#endif

std::array<uint8_t, 32> sha256(llvm::ArrayRef<uint8_t> Data) {
#ifdef NEVERD_SHA_EXTENSIONS
  static const bool HasTargetFeatures = hasTargetFeatures();
  if (HasTargetFeatures)
    return sha256WithExtensions(Data);
#endif
  return llvm::SHA256::hash(Data);
}

} // namespace neverd
