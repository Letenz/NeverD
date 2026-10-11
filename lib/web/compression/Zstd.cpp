//===- Zstd.cpp - In-process Zstandard decoder selection ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Prefer LLVM Support, with an optional pinned native SDK when LLVM omits it.
///
//===----------------------------------------------------------------------===//

#include "Zstd.h"

#include "llvm/Support/Compression.h"
#include "llvm/Support/Error.h"

#ifdef NEVERD_WEB_ZSTD
#include <zstd.h>
static_assert(ZSTD_VERSION_NUMBER == 10507,
              "The Web native Zstandard profile requires SDK 1.5.7");
#endif

namespace neverd::web::compression {
bool zstdAvailable() {
  if (llvm::compression::zstd::isAvailable())
    return true;
#ifdef NEVERD_WEB_ZSTD
  return ZSTD_versionNumber() == 10507;
#else
  return false;
#endif
}

std::optional<size_t> decompressZstd(std::string_view Input,
                                     std::span<char> Output) {
  if (llvm::compression::zstd::isAvailable()) {
    size_t Written = Output.size();
    const llvm::ArrayRef<uint8_t> Bytes(
        reinterpret_cast<const uint8_t *>(Input.data()), Input.size());
    if (auto E = llvm::compression::zstd::decompress(
            Bytes, reinterpret_cast<uint8_t *>(Output.data()), Written)) {
      llvm::consumeError(std::move(E));
      return std::nullopt;
    }
    return Written;
  }
#ifdef NEVERD_WEB_ZSTD
  if (ZSTD_versionNumber() == 10507) {
    const auto Written = ZSTD_decompress(Output.data(), Output.size(),
                                         Input.data(), Input.size());
    if (!ZSTD_isError(Written))
      return Written;
  }
#endif
  return std::nullopt;
}
} // namespace neverd::web::compression
