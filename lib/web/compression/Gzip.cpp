//===- Gzip.cpp - Bounded native gzip stream decoding -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit in-process decoder; failed output never becomes selectable.
///
//===----------------------------------------------------------------------===//

#include "Gzip.h"

#include "../BlobStore.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#ifdef NEVERD_WEB_ZLIB
#include <zlib.h>
#endif

namespace neverd::web::compression {
bool gzipAvailable() {
#ifdef NEVERD_WEB_ZLIB
  return true;
#else
  return false;
#endif
}

Blob decodeGzip(const Blob &Input, uint64_t MaxOutput) {
#ifndef NEVERD_WEB_ZLIB
  throw Error("gzip_decoder_unavailable");
#else
  if (Input.size() > ::neverd::limits::web::kGzipMaxInput ||
      MaxOutput > ::neverd::limits::web::kGzipDecodeGzipMaxOutputLimit)
    throw Error("gzip_byte_budget_exceeded");
  struct Inflater {
    struct alignas(std::max_align_t) Allocation {
      size_t Size;
    };
    z_stream Stream{};
    uint64_t Memory = 0;
    bool Initialized = false;
    static void *allocate(void *Opaque, uInt Count, uInt Size) {
      auto &Self = *static_cast<Inflater *>(Opaque);
      const uint64_t Bytes = uint64_t(Count) * Size + sizeof(Allocation);
      if (Bytes > ::neverd::limits::web::kMaxGzipAllocatorBytes - Self.Memory)
        return nullptr;
      auto *Block = static_cast<Allocation *>(std::calloc(1, size_t(Bytes)));
      if (!Block)
        return nullptr;
      Block->Size = size_t(Bytes);
      Self.Memory += Bytes;
      return Block + 1;
    }
    static void release(void *Opaque, void *Address) {
      if (!Address)
        return;
      auto &Self = *static_cast<Inflater *>(Opaque);
      auto *Block = static_cast<Allocation *>(Address) - 1;
      Self.Memory -= Block->Size;
      std::free(Block);
    }
    Inflater() {
      Stream.opaque = this;
      Stream.zalloc = allocate;
      Stream.zfree = release;
      if (inflateInit2(&Stream, 15 + 16) != Z_OK)
        throw Error("gzip_decoder_unavailable");
      Initialized = true;
    }
    ~Inflater() {
      if (Initialized)
        inflateEnd(&Stream);
    }
  } Decoder;
  auto &S = Decoder.Stream;
  BlobStore Store(MaxOutput);
  std::string Chunk;
  std::array<char, BlobTransferBytes> Output;
  uint64_t Read = 0, Produced = 0, Calls = 0;
  const auto Deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(
          ::neverd::limits::web::kGzipDecodeGzipDeadlineSeconds);
  for (;;) {
    if (++Calls > ::neverd::limits::web::kMaxGzipInflateCalls ||
        std::chrono::steady_clock::now() > Deadline)
      throw Error("gzip_work_budget_exceeded");
    if (!S.avail_in && Read < Input.size()) {
      Chunk =
          Input.read(Read, std::min(BlobTransferBytes, Input.size() - Read));
      Read += Chunk.size();
      S.next_in = reinterpret_cast<Bytef *>(Chunk.data());
      S.avail_in = uInt(Chunk.size());
    }
    const auto Before = S.avail_in;
    S.next_out = reinterpret_cast<Bytef *>(Output.data());
    S.avail_out = uInt(Output.size());
    const auto Status = inflate(&S, Z_NO_FLUSH);
    const auto Count = Output.size() - S.avail_out;
    if (Count > MaxOutput - Produced)
      throw Error("gzip_output_budget_exceeded");
    Produced += Count;
    Store.append({Output.data(), Count});
    if (Status == Z_STREAM_END) {
      if (S.avail_in || Read != Input.size())
        throw Error("gzip_trailing_data");
      break;
    }
    if (Status != Z_OK || (!Count && Before == S.avail_in))
      throw Error("gzip_invalid_stream");
  }
  Store.seal();
  return Store.whole();
#endif
}
} // namespace neverd::web::compression
