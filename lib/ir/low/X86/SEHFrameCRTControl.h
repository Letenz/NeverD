//===- SEHFrameCRTControl.h - Win64 CRT output bounds -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounds a floating-point control invocation's actual caller-frame output.

#ifndef NEVERD_IR_LOW_X86_SEHFRAMECRTCONTROL_H
#define NEVERD_IR_LOW_X86_SEHFRAMECRTCONTROL_H

#include <cstdint>
#include <optional>
#include <set>

namespace neverd {
struct TargetRegInfo;
namespace seh_frame {
struct State;

std::optional<int64_t>
checkedX64ControlFPOutput(const State &Caller, const TargetRegInfo &TRI,
                          int64_t HomeEnd,
                          const std::set<int64_t> &ReturnSlots);

} // namespace seh_frame
} // namespace neverd

#endif
