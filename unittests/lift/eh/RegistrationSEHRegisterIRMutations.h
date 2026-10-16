//===- RegistrationSEHRegisterIRMutations.h - Register edits --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tests refusal of damaged finally register transport contracts.

#ifndef NEVERD_TEST_REGISTRATIONSEHREGISTERIRMUTATIONS_H
#define NEVERD_TEST_REGISTRATIONSEHREGISTERIRMUTATIONS_H

namespace llvm {
class Function;
}
namespace neverd {
struct ExceptionFunction;
struct BinaryImage;
namespace registration_test {
void checkSEHRegisterEdits(const llvm::Function &Parent,
                           const ExceptionFunction &Source,
                           const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd

#endif // NEVERD_TEST_REGISTRATIONSEHREGISTERIRMUTATIONS_H
