//===- RegistrationStateTestUtils.h - x86 EH state fixtures -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSTATETESTUTILS_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSTATETESTUTILS_H

#include "neverd/ir/RegistrationCall.h"
#include "neverd/ir/low/LowIR.h"

#include <initializer_list>

namespace neverd::registration_test {

void emitOp(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs,
            NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default);
void addSlotStore(LowBlock &Block, int32_t Value, uint16_t Width = 4);
/// Append a physical tail jump and move its continuation. Mutation zero is
/// valid; the remaining eight variants corrupt only the tail's source proof.
void addFinallyTailJump(LowBlock &Call, LowBlock &Resume,
                        std::vector<RegistrationSEHControlContract> &Controls,
                        unsigned Mutation);
/// Preserve the physical CALL boundary while adding an unproved later effect.
void addFinallyCallTrailingEffect(LowBlock &Call);
LowFunc makeBranchingFrame();
LowFunc makeCookieFrame(bool GS = false);
LowFunc makeJumpFrame(unsigned Mutation = 0);
void initializeEH4JumpFrame(LowFunc &Function, bool GS = false);
LowFunc makeCxxCatchContinuation(bool IncludeResume = true);

} // namespace neverd::registration_test

#endif
