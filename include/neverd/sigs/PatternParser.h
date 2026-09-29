//===- PatternParser.h - FLIRT .pat text format parser --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parser for FLIRT .pat (pattern) text files. Each line describes one
/// function signature with leading hex bytes, mask, CRC, length, and
/// one or more public function names with offsets.
///
/// Format:
///   <hex_pattern> <crc_len> <crc16> <total_len> [:offset name]... [tail]
///
/// Example:
///   558BEC83EC..5356578B7D08 0A 1234 0042 :0000 _main :0010 _helper
///
/// A signature directory holds hundreds of megabytes of such lines, so a
/// text is cut into chunks of whole lines that are parsed in parallel into
/// packed modules (see \ref PatternChunk); the result is the same as parsing
/// it line by line.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_PATTERNPARSER_H
#define NEVERD_SIGS_PATTERNPARSER_H

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
namespace sigs {

/// A run of whole lines of one pattern text, parsed on its own so that the
/// chunks of a large text, or of many texts, can be parsed in parallel.
struct PatternChunk {
  /// The lines, each with its line feed except perhaps the text's last.
  llvm::StringRef Text;
  /// Where the bytes of its modules go, and how many fit; see
  /// PatternParser::reserveBytes.
  uint8_t *Bytes = nullptr;
  size_t ByteCapacity = 0;
  /// The modules its lines state, in order.  They point into \ref Bytes and
  /// \ref Names, not into \ref Text, which may go away once the chunk is
  /// parsed.
  std::vector<StoredModule> Modules;
  PatternNames Names;
  /// How many lines it holds, which is what numbers the lines of the chunks
  /// after it.
  size_t Lines = 0;
  /// The first malformed line, counted from the chunk's first; parsing the
  /// chunk stops there.
  std::optional<size_t> ErrorLine;
  std::string Error;
};

class PatternParser {
public:
  /// Parse a single .pat line into a PatternModule.
  static llvm::Expected<PatternModule> parseLine(llvm::StringRef Line);

  /// The size of the chunks a text is cut into: small enough that one large
  /// file keeps every worker busy, large enough that a chunk is hundreds of
  /// lines.
  static constexpr size_t DefaultChunkBytes = SignatureLimits::ParseChunkBytes;

  /// Parse pattern text as one transaction. Comments, blank lines, and
  /// separators are ignored; every other line must be a valid module.
  static llvm::Expected<std::vector<PatternModule>>
  parseText(llvm::StringRef Text, size_t ChunkBytes = DefaultChunkBytes);

  /// Parse a .pat file, returning all valid modules.
  static llvm::Expected<std::vector<PatternModule>>
  parseFile(const std::filesystem::path &Path);

  /// Cut \p Text into chunks of whole lines, each at least \p ChunkBytes
  /// long except the last.
  static std::vector<PatternChunk>
  splitChunks(llvm::StringRef Text, size_t ChunkBytes = DefaultChunkBytes);

  /// Allocate the memory the modules of \p Chunks keep their bytes in, and
  /// give each chunk its part.  The modules parsed into it point into it, so
  /// it must outlive them.
  ///
  /// It is one allocation, for as many bytes as the chunks' lines could state
  /// (a line stating N bytes is at least 2N + 11 characters, and its module
  /// takes N bytes and N / 8 bytes of stated bits, rounded up).  What the
  /// lines do not use is never touched, and parsing in parallel does not
  /// contend for the allocator.
  static std::unique_ptr<uint8_t[]>
  reserveBytes(llvm::MutableArrayRef<PatternChunk> Chunks);

  /// Parse every chunk, on the worker threads when there are enough of them,
  /// calling \p Parsed with each chunk's index once that chunk is done.  Its
  /// bytes must have been reserved.
  static void parseChunks(llvm::MutableArrayRef<PatternChunk> Chunks,
                          llvm::function_ref<void(size_t)> Parsed = nullptr);

  /// The first malformed line of \p Chunks, the consecutive chunks of one
  /// text, reported by its line number in that text.
  static llvm::Error firstError(llvm::ArrayRef<PatternChunk> Chunks);

  /// The module \p Module packs, as a value.
  static PatternModule toPatternModule(const StoredModule &Module);

private:
  static void parseChunk(PatternChunk &Chunk);
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_PATTERNPARSER_H
