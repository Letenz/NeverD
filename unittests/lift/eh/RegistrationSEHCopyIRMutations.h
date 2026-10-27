//===- RegistrationSEHCopyIRMutations.h - Callback copy edits ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATIONSEHCOPYIRMUTATIONS_H
#define NEVERD_TEST_REGISTRATIONSEHCOPYIRMUTATIONS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
void checkSEHCopyEdits(const llvm::Function &Parent,
                       const ExceptionFunction &Source,
                       const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_TEST_REGISTRATIONSEHCOPYIRMUTATIONS_H
