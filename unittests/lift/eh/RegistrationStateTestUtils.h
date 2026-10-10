//===- RegistrationStateTestUtils.h - x86 EH state fixtures -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSTATETESTUTILS_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSTATETESTUTILS_H

#include "neverd/ir/low/LowIR.h"

#include <initializer_list>

namespace neverd::registration_test {

void emitOp(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs,
            NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default);
void addSlotStore(LowBlock &Block, int32_t Value, uint16_t Width = 4);
LowFunc makeBranchingFrame();
LowFunc makeCxxCatchContinuation(bool IncludeResume = true);

} // namespace neverd::registration_test

#endif
