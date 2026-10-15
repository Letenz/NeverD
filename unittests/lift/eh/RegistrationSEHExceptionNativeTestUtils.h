//===- RegistrationSEHExceptionNativeTestUtils.h - SEH edits -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATIONSEHEXCEPTIONNATIVETESTUTILS_H
#define NEVERD_TEST_REGISTRATIONSEHEXCEPTIONNATIVETESTUTILS_H

namespace llvm {
class Function;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
struct RegistrationStateAnalysis;
namespace registration_test {
void checkSEHExceptionEdits(const llvm::Function &Parent,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image,
                            const RegistrationStateAnalysis &States);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_TEST_REGISTRATIONSEHEXCEPTIONNATIVETESTUTILS_H
