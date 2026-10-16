//===- RegistrationSEHParentExitIRMutations.h - Parent exit edits -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares independent parent context mutation checks.
#ifndef NEVERD_REGISTRATIONSEHPARENTEXITIRMUTATIONS_H
#define NEVERD_REGISTRATIONSEHPARENTEXITIRMUTATIONS_H

namespace llvm {
class Function;
class Instruction;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct ExceptionFunction;
namespace registration_test {
/// Insert unrelated setup mutations before the contiguous native capture.
llvm::Instruction *sourceSetupMutationPoint(llvm::Function &Function);
void checkSEHParentExitEdits(const llvm::Function &Parent,
                             const ExceptionFunction &Source,
                             const BinaryImage &Image);
} // namespace registration_test
} // namespace neverd
#endif
