//===- RegistrationSEHNullAccessTestUtils.h - Null edits -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATIONSEHNULLACCESSTESTUTILS_H
#define NEVERD_TEST_REGISTRATIONSEHNULLACCESSTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
struct MedFunc;
namespace registration_test {
void checkSEHNullReadEdits(const llvm::Function &Parent, const MedFunc &Med,
                           const ExceptionFunction &Source,
                           const BinaryImage &Image);
}
} // namespace neverd

#endif // NEVERD_TEST_REGISTRATIONSEHNULLACCESSTESTUTILS_H
