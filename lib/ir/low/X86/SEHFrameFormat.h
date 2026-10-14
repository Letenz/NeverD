//===- SEHFrameFormat.h - Source-written Win64 formats --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares checked scalar formats in live source-written frame storage.

#ifndef NEVERD_IR_LOW_X86_SEHFRAMEFORMAT_H
#define NEVERD_IR_LOW_X86_SEHFRAMEFORMAT_H

#include <cstddef>
#include <cstdint>
#include <set>

namespace neverd::seh_frame {
struct State;

/// Require every byte through NUL to have the same reaching value, outside
/// runtime scratch/home storage and active return PCs, within the live frame.
bool checkedX64FrameFormat(const State &Caller, int64_t Address,
                           int64_t HomeEnd,
                           const std::set<int64_t> &ReturnSlots,
                           size_t &Remaining);

} // namespace neverd::seh_frame

#endif
