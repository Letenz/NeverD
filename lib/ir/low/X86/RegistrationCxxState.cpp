//===- RegistrationCxxState.cpp - Current-image C++ call evidence
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/RegistrationCxxState.h"

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace neverd {

RegistrationStateAnalysis analyzeCheckedX86CxxStates(
    const BinaryImage &Image, const LowFunc &Function,
    const std::vector<RegistrationCalleeFrameContract> *Callees,
    const std::vector<RegistrationCleanupFrameContract> *Cleanups,
    const std::vector<RegistrationCalleeStackContract> *Stacks,
    const std::vector<RegistrationLocalUnwindContract> *LocalUnwinds,
    const std::vector<RegistrationSEHControlContract> *SEHControls) {
  if (!Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return {};
  const auto &EH = *Function.ExceptionMetadata;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  const auto Prologue = coff_loader::getCheckedX86CxxPrologueCall(Image, EH);
  std::vector<coff_loader::X86CxxEpilogueCall> Epilogues;
  if (Prologue)
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.Addr != Prologue->CallVA)
          if (const auto Exit = coff_loader::getCheckedX86CxxEpilogueCall(
                  Image, EH, Op.Addr)) {
            if (Epilogues.size() == limits::kMaxRegistrationEHRecords)
              return {};
            Epilogues.push_back(*Exit);
          }
  const va_t Cookie =
      Image.Base <= UINT32_MAX && Image.DynInfo.SecurityCookieRVA &&
              Image.DynInfo.SecurityCookieRVA <= UINT32_MAX - Image.Base
          ? Image.Base + Image.DynInfo.SecurityCookieRVA
          : 0;
  return registration_state::RegistrationStateSolver(
             Function, Cookie, 0, Callees, Cleanups, Stacks, LocalUnwinds,
             SEHControls, nullptr, nullptr, nullptr,
             Handler ? &*Handler : nullptr, Prologue ? &*Prologue : nullptr,
             &Epilogues)
      .run();
}

} // namespace neverd
