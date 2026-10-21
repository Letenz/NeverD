//===- Brotli.cpp - In-process budgeted Brotli decoder --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pinned static decoder with private allocation accounting and exact framing.
///
//===----------------------------------------------------------------------===//

#include "Brotli.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <memory>

#ifdef NEVERD_WEB_BROTLI
#include <brotli/decode.h>
#endif

namespace neverd::web::compression {
bool brotliAvailable() {
#ifdef NEVERD_WEB_BROTLI
  return BrotliDecoderVersion() == 0x1002000;
#else
  return false;
#endif
}
#ifdef NEVERD_WEB_BROTLI
namespace {
struct Allocation {
  uint64_t Used = 0, Limit;
  bool Refused = false;
  struct alignas(std::max_align_t) Header {
    size_t Size;
  };
  static void *allocate(void *Opaque, size_t Size) noexcept {
    auto &A = *static_cast<Allocation *>(Opaque);
    if (Size > A.Limit - A.Used || sizeof(Header) > A.Limit - A.Used - Size) {
      A.Refused = true;
      return nullptr;
    }
    auto *H = static_cast<Header *>(std::malloc(sizeof(Header) + Size));
    if (!H) {
      A.Refused = true;
      return nullptr;
    }
    H->Size = sizeof(Header) + Size;
    A.Used += H->Size;
    return H + 1;
  }
  static void release(void *Opaque, void *Pointer) noexcept {
    if (!Pointer)
      return;
    auto &A = *static_cast<Allocation *>(Opaque);
    auto *H = static_cast<Header *>(Pointer) - 1;
    A.Used -= H->Size;
    std::free(H);
  }
};
} // namespace
#endif
std::string decodeBrotli(std::string_view Input, uint64_t MaxOutput,
                         uint64_t Memory, uint64_t &Calls, uint64_t MaxCalls,
                         std::chrono::steady_clock::time_point Deadline) {
  if (!brotliAvailable())
    throw Error("brotli_decoder_unavailable");
  if (Input.size() > ::neverd::limits::web::kBrotliDecodeBrotliInputSizeLimit ||
      MaxOutput > ::neverd::limits::web::kBrotliDecodeBrotliMaxOutputLimit ||
      Memory > 64ULL * 1024 * 1024 ||
      MaxCalls > ::neverd::limits::web::kBrotliDecodeBrotliMaxCallsLimit)
    throw Error("brotli_invalid_limits");
#ifdef NEVERD_WEB_BROTLI
  Allocation A{0, Memory};
  std::unique_ptr<BrotliDecoderState, decltype(&BrotliDecoderDestroyInstance)>
      S(BrotliDecoderCreateInstance(Allocation::allocate, Allocation::release,
                                    &A),
        BrotliDecoderDestroyInstance);
  if (!S)
    throw Error("brotli_memory_budget_exceeded");
  std::string Output;
  size_t At = 0;
  constexpr uint8_t Empty = 0;
  const auto *InputBytes =
      Input.empty() ? &Empty : reinterpret_cast<const uint8_t *>(Input.data());
  while (true) {
    if (Calls >= MaxCalls)
      throw Error("brotli_call_budget_exceeded");
    if (std::chrono::steady_clock::now() >= Deadline)
      throw Error("brotli_time_budget_exceeded");
    ++Calls;
    size_t In = std::min<size_t>(
        ::neverd::limits::web::kBrotliDecodeBrotliInMinArgument1,
        Input.size() - At);
    const auto *NextIn = InputBytes + At;
    const auto Before = In;
    std::array<uint8_t, 4096> Buffer{};
    // One sentinel byte detects output beyond the admitted bound, including
    // the zero-output limit. Never allocate a claimed decoded size.
    size_t Out =
        std::min<uint64_t>(Buffer.size(), MaxOutput - Output.size() + 1);
    auto *NextOut = Buffer.data();
    const auto Capacity = Out;
    const auto Result = BrotliDecoderDecompressStream(S.get(), &In, &NextIn,
                                                      &Out, &NextOut, nullptr);
    At += Before - In;
    const auto Produced = Capacity - Out;
    if (A.Refused)
      throw Error("brotli_memory_budget_exceeded");
    if (Produced > MaxOutput - Output.size())
      throw Error("brotli_output_budget_exceeded");
    Output.append(reinterpret_cast<const char *>(Buffer.data()), Produced);
    if (Result == BROTLI_DECODER_RESULT_SUCCESS) {
      if (At != Input.size())
        throw Error("brotli_trailing_bytes");
      return Output;
    }
    if (Result == BROTLI_DECODER_RESULT_ERROR)
      throw Error("brotli_invalid_stream");
    if (Result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && At == Input.size())
      throw Error("brotli_truncated_stream");
    if (!Produced && In == Before)
      throw Error("brotli_no_progress");
  }
#else
  throw Error("brotli_decoder_unavailable");
#endif
}
} // namespace neverd::web::compression
