//===- ISAIdentify.h - The instruction set a binary file holds --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A binary file names no processor.  Its bytes still show one: each
// instruction set leaves its own statistics of which byte follows which, and
// some files start with a structure -- a Cortex-M vector table -- that names
// the processor and where the code runs.  RawISA.def lists the sets the
// model knows, RawFingerprints.def the structures.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_RAW_ISAIDENTIFY_H
#define NEVERD_LOADER_RAW_ISAIDENTIFY_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// An instruction set a binary file's bytes look like.
struct ISAGuess {
  /// The set's name in RawISA.def, such as "aarch64".
  llvm::StringRef ISA;
  /// The name the user reads, such as "ARM64 (AArch64)".
  llvm::StringRef Name;
  /// The RawLoader.def processor NeverD reads the set as; empty when NeverD
  /// cannot decode it.
  llvm::StringRef Processor;
  /// The share of the code windows' votes the set took.
  double Share = 0;
};

/// A structure at the start of a binary file that names its processor and
/// where its code runs.
struct RawFingerprint {
  llvm::StringRef Name;
  llvm::StringRef Processor;
  va_t Entry = 0;
  va_t Base = 0;
};

/// What a binary file's bytes show of the instruction set they hold.
struct ISAIdentification {
  /// What the bytes settle; RawISAVerdict.def describes each.
  enum class Verdict {
#define NEVERD_RAW_VERDICT(Id, Name) Id,
#include "neverd/loader/Raw/RawISAVerdict.def"
  };
  Verdict Outcome = Verdict::NoCode;
  /// The likeliest sets, most votes first: one per family, or both sets of
  /// the leading family when its width is unclear.
  std::vector<ISAGuess> Guesses;
  /// The share of the windows read that look like code.
  double CodeShare = 0;
  /// The bytes the code's instructions align to, and where they start
  /// modulo that: the file offset that maps them to aligned addresses.
  unsigned CodeUnit = 1;
  unsigned CodeOffset = 0;
  /// For a family whose width the instructions tell: of the instructions
  /// read, the share only the 64-bit set has, as the median window holds.
  std::optional<double> WideShare;
  /// A structure that names the processor, which outranks the statistics.
  std::optional<RawFingerprint> Fingerprint;

  /// The name the C API reports Outcome with: "settled", "unclear", ...
  llvm::StringRef outcomeName() const;
  /// The processor a binary file is read as unasked: the fingerprint's, or
  /// the settled set's when NeverD decodes it; empty when neither names one.
  llvm::StringRef detectedProcessor() const;
  /// Why, for the user: "ARM64 (AArch64) in 97% of the code", "a Cortex-M
  /// vector table", or what the bytes look like instead.
  std::string describe() const;
};

/// Identify the instruction set \p Bytes, a binary file's contents, hold.
/// Windows of padding, text or compressed data are skipped; each other
/// window votes for the set whose statistics explain it best, weighted by
/// how much better than any set of another family.  The family that takes
/// the votes, by enough of the file, settles it; its instructions then tell
/// where they start and, for a family of a 32-bit and a 64-bit set, which.
/// Reads a bounded number of windows, spread over a large file.
ISAIdentification identifyISA(llvm::ArrayRef<uint8_t> Bytes);

/// Of the instructions in \p Code, read from \p Offset on the way \p Family
/// encodes them, the share only the family's 64-bit set has; none when the
/// family is not one of a 32-bit and a 64-bit set (RawISA.def).
std::optional<double> readWideShare(llvm::StringRef Family,
                                    llvm::ArrayRef<uint8_t> Code,
                                    unsigned Offset = 0);

/// The structure at the start of \p Bytes that names its processor, if any.
std::optional<RawFingerprint> readRawFingerprint(llvm::ArrayRef<uint8_t> Bytes);

} // namespace neverd

#endif // NEVERD_LOADER_RAW_ISAIDENTIFY_H
