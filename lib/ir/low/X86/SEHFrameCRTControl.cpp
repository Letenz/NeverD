//===- SEHFrameCRTControl.cpp - Win64 floating-point control effects ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks the bounded memory effect of an authenticated _controlfp_s call.

#include "SEHFrameCRTControl.h"

#include "../SEHFrameState.h"

#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd::seh_frame {

std::optional<int64_t>
checkedX64ControlFPOutput(const State &Caller, const TargetRegInfo &TRI,
                          int64_t HomeEnd,
                          const std::set<int64_t> &ReturnSlots) {
  if (TRI.TheArch != Arch::X64 || TRI.PointerSize != 8 ||
      TRI.Win64ParamRegs.size() < 3 || Caller.EscapedFrame)
    return std::nullopt;
  const auto Output = Caller.get(NdVar::reg(TRI.Win64ParamRegs[0], 8));
  const auto Mask = Caller.constant(NdVar::reg(TRI.Win64ParamRegs[2], 8));
  // Admit queries and exception-mask changes only. Precision-control bits on
  // x64 can call the application-defined invalid-parameter handler, whose
  // memory effects are not supplied by this CRT contract. No interpretation
  // of the current MXCSR or the returned control word is needed here.
  if (!Output || !Mask || (*Mask & ~uint64_t{0x8001f}) || *Output < HomeEnd ||
      *Output > -4 || llvm::any_of(ReturnSlots, [&](int64_t Slot) {
        return Slot > INT64_MAX - 8 ||
               (*Output < Slot + 8 && Slot < *Output + 4);
      }))
    return std::nullopt;
  return Output;
}

} // namespace neverd::seh_frame
