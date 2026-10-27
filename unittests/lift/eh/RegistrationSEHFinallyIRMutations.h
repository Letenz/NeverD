//===- RegistrationSEHFinallyIRMutations.h - Finally edits ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise installation refusal for edited ordinary finally invocations.

#ifndef NEVERD_TEST_REGISTRATIONSEHFINALLYIRMUTATIONS_H
#define NEVERD_TEST_REGISTRATIONSEHFINALLYIRMUTATIONS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
void checkSEHFinallyEdits(const llvm::Function &Parent,
                          const ExceptionFunction &Source,
                          const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_TEST_REGISTRATIONSEHFINALLYIRMUTATIONS_H
