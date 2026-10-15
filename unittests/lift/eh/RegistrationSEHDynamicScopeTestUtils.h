//===- RegistrationSEHDynamicScopeTestUtils.h - Scope edits ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TEST_REGISTRATIONSEHDYNAMICSCOPE_H
#define NEVERD_TEST_REGISTRATIONSEHDYNAMICSCOPE_H
namespace llvm {
class Function;
}
namespace neverd {
struct ExceptionFunction;
struct BinaryImage;
namespace registration_test {
void checkDynamicSEHScopeEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image);
void checkDynamicSEHCallEdits(const llvm::Function &Parent,
                              const ExceptionFunction &Source,
                              const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd
#endif // NEVERD_TEST_REGISTRATIONSEHDYNAMICSCOPE_H
