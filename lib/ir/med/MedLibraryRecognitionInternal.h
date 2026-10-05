//===- MedLibraryRecognitionInternal.h - Bounded family matchers --------===//
#ifndef NEVERD_MED_LIBRARY_RECOGNITION_INTERNAL_H
#define NEVERD_MED_LIBRARY_RECOGNITION_INTERNAL_H
#include "neverd/ir/med/MedLibraryRecognition.h"

#include <set>
namespace neverd {
std::optional<sigs::LibraryRecognition> recognizeMedComLibraryOperation(
    const MedFunc &, const BinaryImage &, const sigs::LibraryFeaturePack &,
    const sigs::LibraryFeatureRule &, llvm::ArrayRef<MedLibraryReceiver>,
    const std::set<std::string> &, size_t &, bool &);
}
#endif
