//===- RegistrationCxxContinuationTestUtils.h - Catch resumes -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_REGISTRATIONCXXCONTINUATIONTESTUTILS_H
#define NEVERD_REGISTRATIONCXXCONTINUATIONTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
void checkCxxContinuationEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_REGISTRATIONCXXCONTINUATIONTESTUTILS_H
