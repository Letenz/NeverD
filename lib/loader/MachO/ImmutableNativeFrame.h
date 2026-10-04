#ifndef NEVERD_LOADER_MACHO_IMMUTABLENATIVEFRAME_H
#define NEVERD_LOADER_MACHO_IMMUTABLENATIVEFRAME_H

#include "SourceLocalCall.h"

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/loader/MachO/ImmutableNativeCalls.h"

namespace neverd {
enum class ImmutableNativeFrameTerminators { ExcludeOpaque, MatchDecoder };

/// Loader-owned authentication only. Byte identities and frame effects remain
/// in SourceFrameAnalysis and the individual effect owners.
/// Opaque exits remain excluded unless a consumer separately proves their
/// control and observation semantics, then opts into decoder-only matching.
bool immutableNativeFrameMachineMatches(
    const BinaryImage &Image, const LowFunc &Function, size_t &Budget,
    ImmutableNativeFrameTerminators =
        ImmutableNativeFrameTerminators::ExcludeOpaque);

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
/// One bounded, freshly authenticated private-frame definition query owner.
/// Consumers supply only independently established call contracts; this never
/// recursively discovers targets or grants publication authority.
struct AuthenticatedSourceFrameLoads {
  const BinaryImage &Image;
  const LowFunc &Function;
  const SourceLocalCalls &DirectCalls;
  const std::map<va_t, SourceFunctionTypeHint> *NativeCallees;
  size_t &Budget;
  std::optional<bool> MachineMatches;
  size_t QueriesLeft = 64;
  bool Exhausted = false;
  const std::map<va_t, ImmutableNativeCallTarget> *KnownTargets = nullptr;
  std::optional<ImmutableNativeFrameCalls> Contracts;
  std::map<std::pair<int, size_t>, std::optional<SourceFrameLoadDefinition>>
      Cache;

  void beginRound(const std::map<va_t, ImmutableNativeCallTarget> &Known);
  const NativeSourceCalls &calls();
  std::optional<SourceFrameLoadDefinition> load(const LowBlock &Block,
                                                size_t Index);
};

} // namespace neverd
#endif
