//===- MedLibraryRecognition.h - Library expression analysis -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDLIBRARYRECOGNITION_H
#define NEVERD_IR_MED_MEDLIBRARYRECOGNITION_H

#include "neverd/ir/med/MedIR.h"
#include "neverd/sigs/LibraryRecognition.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd {

/// Operation-scoped snapshot of stated function identities. Build once per
/// image analysis, before parallel workers inspect the same immutable image.
class MedLibraryIdentityIndex {
public:
  explicit MedLibraryIdentityIndex(const BinaryImage &Image);
  llvm::ArrayRef<std::string> names(const BinaryImage &Image, va_t Entry) const;

private:
  const BinaryImage *Image;
  std::map<va_t, std::vector<std::string>> Names;
};

/// An independently authenticated type on a physical function-entry input.
/// The producer must bind the original declaration/location to this image;
/// a guessed type, a display rename or a matching layout is not such a fact.
struct MedLibraryReceiver {
  uint64_t Register = 0;
  std::string Type;
  uint32_t ObjectBytes = 0;
  std::string Evidence;
};

struct MedLibraryRecognitionResult {
  std::vector<sigs::LibraryRecognition> Matches;
  bool BudgetExhausted = false;
};

/// Inspect unchanged SSA. Unknown definitions, widths, identities, ordering,
/// or conflicting candidates produce no annotation. The budget is shared by
/// all candidate rules in this function; exhaustion publishes no partial set.
MedLibraryRecognitionResult recognizeMedLibraryOperations(
    const MedFunc &Function, const BinaryImage &Image,
    const std::map<std::string, sigs::LibraryFeaturePack> &Packs,
    llvm::ArrayRef<MedLibraryReceiver> Receivers = {}, size_t MaxWork = 250000,
    const MedLibraryIdentityIndex *Identities = nullptr);

} // namespace neverd

#endif
