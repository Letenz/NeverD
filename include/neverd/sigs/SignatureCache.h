//===- SignatureCache.h - Parsed signature files, kept on disk --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A cache of parsed signature files.  A signature directory holds hundreds
/// of megabytes of pattern text, and parsing it is the one part of loading
/// signatures that does not depend on the image: the cache lets a file be
/// parsed once rather than on every load.
///
/// An entry holds the modules one pattern file parses to, laid out so that
/// loading it maps the entry and points the modules into the mapping: no
/// byte is decoded, and no pattern byte or name is copied.  An entry is
/// named after its source's path, and belongs to the source as it was: its
/// size, modification time and file identity, and the format and the NeverD
/// version that wrote the entry, have to match.  Anything that does not --
/// a changed source, an entry another version wrote, a truncated or damaged
/// one -- is a miss, and the file is parsed; a write that fails is dropped.
/// The cache never changes what a load returns, only how long it takes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_SIGNATURECACHE_H
#define NEVERD_SIGS_SIGNATURECACHE_H

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
namespace sigs {

class SignatureCache {
public:
  /// What makes a source file the one an entry was made from.
  struct SourceIdentity {
    uint64_t Size = 0;
    /// The modification time, in nanoseconds since the epoch.
    int64_t Modified = 0;
    uint64_t Device = 0;
    uint64_t File = 0;

    bool operator==(const SourceIdentity &) const = default;
  };

  /// The modules an entry holds.  They point into \ref Mapping and into
  /// \ref Names, whose text is in the mapping too; moving the entry keeps
  /// both where they are.
  struct Entry {
    std::unique_ptr<llvm::MemoryBuffer> Mapping;
    std::vector<StoredModule> Modules;
    PatternNames Names;
  };

  /// An entry mapped and checked as a whole, whose blocks are yet to be
  /// checked and whose records are yet to be turned into modules.  That work
  /// comes in independent pieces, so that a load can share out the pieces of
  /// every entry it opens among its workers.
  class PendingEntry {
  public:
    /// How many pieces the rest of the work is.
    size_t pieces() const { return BlockCount + NameRuns + ModuleRuns; }

    /// Do piece \p Piece, and say whether the entry passed it.  Different
    /// pieces may run at once.
    bool run(size_t Piece);

    /// The entry, once every piece has run and passed.
    Entry take() && { return std::move(Result); }

  private:
    friend class SignatureCache;
    PendingEntry() = default;

    bool checkBlock(size_t Block) const;
    bool makeNames(size_t Run);
    bool makeModules(size_t Run);

    Entry Result;
    const char *Data = nullptr;
    uint64_t BlockHashes = 0, Payload = 0, PayloadBytes = 0;
    uint64_t ModuleRecords = 0, NameRecords = 0, Blob = 0, Text = 0;
    uint64_t BlobBytes = 0, TextBytes = 0;
    size_t BlockCount = 0, NameRuns = 0, ModuleRuns = 0;
  };

  /// A cache in \p Directory, which keeps sources of at least
  /// \p MinSourceBytes.
  SignatureCache(std::string Directory, uint64_t MinSourceBytes);

  /// The cache the NeverD tools use, from the environment; see
  /// SignatureCache.def.  None when the environment turns it off, or when
  /// the platform names no cache directory.
  static std::optional<SignatureCache> fromEnvironment();

  /// The identity of \p Source as it is now, or none when it cannot be read.
  static std::optional<SourceIdentity>
  identify(const std::filesystem::path &Source);

  /// Map the entry the cache holds for \p Source and check it as a whole,
  /// when \p Identity is the identity of the source it was made from.
  std::optional<PendingEntry> open(const std::filesystem::path &Source,
                                   const SourceIdentity &Identity) const;

  /// The modules the cache holds for \p Source, when \p Identity is the
  /// identity of the source its entry was made from: \ref open and every
  /// piece of the entry, on this thread.
  std::optional<Entry> load(const std::filesystem::path &Source,
                            const SourceIdentity &Identity) const;

  /// Keep \p Modules, which \p Source parsed to when its identity was
  /// \p Identity.  Nothing is kept for a source smaller than the cache's
  /// minimum, or one whose identity has changed since.
  void store(const std::filesystem::path &Source,
             const SourceIdentity &Identity,
             llvm::ArrayRef<StoredModule> Modules) const;

  /// Where the entry for \p Source is, or would be.
  std::optional<std::string>
  entryPath(const std::filesystem::path &Source) const;

  const std::string &directory() const { return Directory; }

private:
  std::string Directory;
  uint64_t MinSourceBytes;
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_SIGNATURECACHE_H
