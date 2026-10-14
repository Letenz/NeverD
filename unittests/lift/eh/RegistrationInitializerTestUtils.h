//===- RegistrationInitializerTestUtils.h - Constructor edits ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Mutation checks for preserved PE32 scalar constructors.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_REGISTRATIONINITIALIZERTESTUTILS_H
#define NEVERD_TESTS_REGISTRATIONINITIALIZERTESTUTILS_H

namespace llvm {
class Function;
}
namespace neverd {
struct BinaryImage;
struct MedFunc;
namespace registration_test {
void checkInitializerEdits(const MedFunc &Med, const llvm::Function &Parent,
                           const BinaryImage &Image);
}
} // namespace neverd
#endif
