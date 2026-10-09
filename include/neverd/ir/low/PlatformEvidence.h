//===- PlatformEvidence.h - What code shows of its platform -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A binary file names no platform, yet its code follows one: System V,
// Windows or Apple calling conventions, thread blocks and system calls.
// Reading that evidence lets a binary file decompile under the conventions
// its code was built for, and tells the user why.  PlatformEvidence.def is
// the clue database.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_PLATFORMEVIDENCE_H
#define NEVERD_IR_LOW_PLATFORMEVIDENCE_H

#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

class Decoder;

/// One kind of evidence of the platform code was built for.
enum class PlatformClue : uint8_t {
#define NEVERD_PLATFORM_CLUE(Id, Platform, Weight, Text) Id,
#include "neverd/ir/low/PlatformEvidence.def"
};

/// What a binary file's code shows of the platform it was built for.
struct PlatformEvidence {
  /// The platform, as the format whose images follow its conventions: ELF
  /// for System V and AAPCS, COFF for Windows, MachO for Apple.
  BinaryFormat Platform = BinaryFormat::ELF;
  /// Whether the evidence decided the platform.  When the code shows too
  /// little, System V -- the conventions most code without a header
  /// follows -- is assumed, and the user is told so.
  bool Decided = false;
  /// How often each clue was seen, the most telling first.
  std::vector<std::pair<PlatformClue, uint64_t>> Clues;

  /// What the evidence was, for the user: "calls set their first argument
  /// in rdi (412); ...", or why System V was assumed.
  std::string describe() const;
};

/// Read the platform \p Img's code was built for from its executable bytes,
/// decoding x86 with \p Dec, which must be initialized for the image.  The
/// reading is bounded: it stops once the evidence is decisive and reads a
/// few megabytes of code at most.
PlatformEvidence readPlatformEvidence(const BinaryImage &Img, Decoder &Dec);

/// What one sighting of \p Clue is, as PlatformEvidence.def words it.
llvm::StringRef getPlatformClueText(PlatformClue Clue);

} // namespace neverd

#endif // NEVERD_IR_LOW_PLATFORMEVIDENCE_H
