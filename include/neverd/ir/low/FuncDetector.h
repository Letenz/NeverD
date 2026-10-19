//===- FuncDetector.h - Function entry-point detection --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares FuncDetector, which discovers function entry points from symbol
/// tables, exports, call-target scanning, and heuristic validation.
/// Architecture-specific scanners live in FuncDetectorX86.cpp,
/// FuncDetectorARM.cpp, and FuncDetectorAArch64.cpp.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_FUNCDETECTOR_H
#define NEVERD_IR_LOW_FUNCDETECTOR_H

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"

#include <set>
#include <vector>

namespace neverd {

class FuncDetector {
public:
  /// Detect function entry points from the binary image.
  std::vector<std::pair<va_t, std::string>> detect(const BinaryImage &Img,
                                                   Decoder &Dec);

private:
  void scanCallTargets(const BinaryImage &Img, Decoder &Dec,
                       std::set<va_t> &Out);
  /// Recover VC6 / MSVC functions that have no PDB symbol: EBP-frame
  /// prologues at any alignment, plus 16-byte-aligned SEH / callee-saved /
  /// stack-adjust starts.  Tagged entries skip the bounded verify walk so a
  /// large body is not dropped for exhausting kMaxVerifyInsns.
  void scanX86UnsymbolizedEntries(const BinaryImage &Img, Decoder &Dec,
                                  std::set<va_t> &Out);
  /// Drop the unsymbolized x86 entries, and the \p Guessed starts the loader
  /// only inferred from padding, that a function with other entry evidence
  /// reaches as its own code: by falling into one, by a conditional branch,
  /// or as a case a pointer table among its code names.  A target only an
  /// unconditional jump reaches stays an entry, since the jump may be a tail
  /// call, and so does a frame prologue right after a call that may not
  /// return.  \p Evidenced holds the entries with other evidence and
  /// \p TableCases each code address naming code with the code it names.
  /// Returns the entries dropped.
  std::set<va_t> rejectX86InteriorEntries(
      const BinaryImage &Img, Decoder &Dec, const std::set<va_t> &Evidenced,
      const std::set<va_t> &Guessed,
      const std::vector<std::pair<va_t, va_t>> &TableCases);
  /// Validate a heuristic entry.  When KeepInconclusive is true, bounded
  /// probes that exhaust their budget remain candidates for the formal audit;
  /// definite decode or mapping failures are still rejected.
  bool verifyFunctionDecode(const BinaryImage &Img, Decoder &Dec, va_t Addr,
                            bool KeepInconclusive = false);

  std::set<va_t> Entries;
  std::set<va_t> UnsymbolizedX86Entries;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_FUNCDETECTOR_H
