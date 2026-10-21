//===- Contents.cpp - Wails payload digest and overlap proof -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify every stored Go toolchain hash without publishing private names or
/// metadata aliases. Exact payload aliases share bounded hash work.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "neverd/web/Limits.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <map>

namespace neverd::web::wails_detail {
void verifyContents(Reader &R) {
  auto &E = R.Evidence;
  std::sort(R.PrivateRanges.begin(), R.PrivateRanges.end(),
            [](Range A, Range B) { return A.Offset < B.Offset; });
  std::vector<Range> Private;
  for (const auto &Range : R.PrivateRanges) {
    R.step();
    if (!Private.empty() && Range.Offset <= Private.back().end()) {
      auto &Last = Private.back();
      Last.Size = std::max(Last.end(), Range.end()) - Last.Offset;
    } else {
      Private.push_back(Range);
    }
  }
  using Key = std::pair<uint64_t, uint64_t>;
  struct Digest {
    std::string Full, Prefix;
  };
  std::map<Key, Digest> Payloads;
  for (const auto &M : E.Members) {
    R.step();
    if (M.Directory)
      continue;
    const Range Data{M.Offset, M.Content.size()};
    const auto End =
        std::upper_bound(Private.begin(), Private.end(), Data.Offset,
                         [](uint64_t At, Range X) { return At < X.Offset; });
    if ((End != Private.end() && overlaps(Data, *End)) ||
        (End != Private.begin() && overlaps(Data, *std::prev(End))))
      throw Error("wails_payload_metadata_overlap");
    Payloads.try_emplace({Data.Offset, Data.Size});
  }
  uint64_t PreviousEnd = 0;
  for (auto &[Key, Value] : Payloads) {
    R.step();
    if (Key.second && Key.first < PreviousEnd)
      throw Error("wails_partial_payload_overlap");
    if (Key.second)
      PreviousEnd = Key.first + Key.second;
    llvm::SHA256 Hash;
    llvm::SHA256 ToolchainHash;
    const bool Streaming =
        Key.second > ::neverd::limits::web::kWailsStreamingThresholdBytes;
    if (Streaming)
      ToolchainHash.update(llvm::StringRef("\1", 1));
    const auto Content = R.Input.Content.slice(Key.first, Key.second);
    for (uint64_t At = 0; At < Content.size();) {
      R.step();
      const auto Count = std::min(BlobTransferBytes, Content.size() - At);
      const auto Bytes = Content.read(At, Count);
      Hash.update(Bytes);
      if (Streaming)
        ToolchainHash.update(Bytes);
      E.HashedBytes += Count * (Streaming ? 2 : 1);
      At += Count;
    }
    const auto Bytes = Hash.final();
    Value.Full = llvm::toHex(Bytes, true);
    if (Streaming) {
      const auto Stored = ToolchainHash.final();
      Value.Prefix.assign(reinterpret_cast<const char *>(Stored.data()), 16);
      ++E.HashedBytes;
    } else {
      Value.Prefix.assign(reinterpret_cast<const char *>(Bytes.data()), 16);
      Value.Prefix[0] = char(uint8_t(Value.Prefix[0]) ^ 0xff);
    }
  }
  for (uint32_t I = 0; I < E.Members.size(); ++I) {
    R.step();
    auto &M = E.Members[I];
    if (M.Directory)
      continue;
    const auto &Hash = Payloads.at({M.Offset, M.Content.size()});
    if (Hash.Prefix != R.ExpectedHashes.at(I))
      throw Error("wails_embed_hash_mismatch");
    M.BlobHash = Hash.Full;
  }
}
} // namespace neverd::web::wails_detail
