//===- MedCallConventionI386.cpp - i386 call arguments -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 cdecl and stdcall pass every argument on the stack.  Compilers give a
// directly called static function a regparm convention (ECX/EDX, or
// EAX/EDX/ECX) instead, whose stack arguments follow the registers it uses;
// a function-pointer call or an imported function always uses the stack, as
// many arguments as its prototype has.
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd {

namespace {
/// A Windows import may be __fastcall (ECX, EDX); an ELF or Mach-O import
/// takes every argument on the stack.
bool importsMayTakeRegisterArguments(const BinaryImage &Img) {
  return Img.abiFormat() == BinaryFormat::COFF;
}
} // namespace

extern const CallArgumentConvention I386CallArguments;
const CallArgumentConvention I386CallArguments = {
    .TheArch = Arch::X86,
    .ImportArgumentsFromPrototype = true,
    .StackArgumentSummary = true,
    .RegparmOnlyForInternalCalls = true,
    .ImportsMayTakeRegisterArguments = importsMayTakeRegisterArguments,
    .StackArgumentsFollowUsedRegisters = true,
    .StackOnlyVariadicCallees = true,
    .RegisterArgumentsFillInOrder = true,
    .ArgumentsFromCallSetup = true,
};

} // namespace neverd
