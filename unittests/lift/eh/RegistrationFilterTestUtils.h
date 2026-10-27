//===- RegistrationFilterTestUtils.h - SEH fixtures -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONFILTERTESTUTILS_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONFILTERTESTUTILS_H

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd::registration_filter_test {
enum class Variant {
  Correlated,
  SameState,
  BothContinue,
  CapturedCursor,
  StoresAroundRaise,
  ReversedEdges,
  NegativeDisposition,
  SkipFilterBeforeResume,
  SkipTwoFiltersBeforeResume,
  TwoResumeMappings,
  SkipFilterAfterCallback,
  SelectedHandler,
  HandlerOnly,
  HandlerCleanup,
  RecurrentSelection,
  UnknownDisposition,
  EscapingSearch,
  OuterHandler,
  BrokenStack,
  RecurrentArm,
  WrongStoreWidth,
  IncompleteCallInstruction,
};

void addStateStore(LowBlock &Block, va_t Address, int32_t Level,
                   RegistrationChainInfo &Chain);
LowFunc filterFrame(Variant V, unsigned Protocol = 0);
RegistrationStateAnalysis analyze(LowFunc F, Variant V, va_t CookieVA = 0x4000,
                                  bool UnwindJump = false);
RegistrationStateAnalysis analyze(Variant V, unsigned Protocol = 0);
LowFunc callbackActivatedFrame();

} // namespace neverd::registration_filter_test

#endif
