//===- RegistrationSEHStateDispatchTestUtils.h - State edits ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks the closed state selection of reconstructed source callbacks.

#ifndef NEVERD_TEST_REGISTRATIONSEHSTATEDISPATCHTESTUTILS_H
#define NEVERD_TEST_REGISTRATIONSEHSTATEDISPATCHTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
struct MedFunc;
namespace registration_test {
void checkSEHStateDispatchEdits(const llvm::Function &Parent,
                                const MedFunc &Med,
                                const ExceptionFunction &Source,
                                const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd

#endif
