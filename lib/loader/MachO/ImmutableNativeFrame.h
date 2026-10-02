#ifndef NEVERD_LOADER_MACHO_IMMUTABLENATIVEFRAME_H
#define NEVERD_LOADER_MACHO_IMMUTABLENATIVEFRAME_H

#include "SourceLocalCall.h"

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/loader/MachO/ImmutableNativeCalls.h"

namespace neverd {
/// Loader-owned authentication only. Byte identities and frame effects remain
/// in SourceFrameAnalysis and the individual effect owners.
bool immutableNativeFrameMachineMatches(const BinaryImage &Image,
                                        const LowFunc &Function,
                                        size_t &Budget);

std::optional<SourceFunctionTypeHint> immutableNativeDirectCallABI(
    const BinaryImage &Image, va_t Target,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees);

struct ImmutableNativeFrameCalls {
  ImmutableNativeFrameCalls() = default;
  ImmutableNativeFrameCalls(const ImmutableNativeFrameCalls &) = delete;
  ImmutableNativeFrameCalls &
  operator=(const ImmutableNativeFrameCalls &) = delete;
  ImmutableNativeFrameCalls(ImmutableNativeFrameCalls &&) = default;
  ImmutableNativeFrameCalls &operator=(ImmutableNativeFrameCalls &&) = default;
  // Contracts point into these owned map nodes; copying would stale them.
  std::map<NativeSourceCallKey, SourceFunctionTypeHint> Signatures;
  NativeSourceCalls Calls;
};

/// Known indirect targets must come from a completed earlier proof round.
/// Never recursively invoke target discovery through a call-effect wrapper.
ImmutableNativeFrameCalls immutableNativeFrameCalls(
    const BinaryImage &Image, const LowFunc &Function,
    const SourceLocalCalls &DirectCalls,
    const std::map<va_t, ImmutableNativeCallTarget> &KnownTargets,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees);
} // namespace neverd
#endif
