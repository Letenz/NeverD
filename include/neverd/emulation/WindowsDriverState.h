//===- WindowsDriverState.h - Owned stopped kernel state -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Kernel-owned resource identity and contents at a stopped invocation.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWSDRIVERSTATE_H
#define NEVERD_EMULATION_WINDOWSDRIVERSTATE_H

#include "neverd/emulation/ProcessRuntimeState.h"

#include <cstdint>
#include <optional>

namespace neverd::emulation {
/// Captured by the kernel owner and transported through a const runtime state.
/// Resource bytes do not establish pointer provenance or authorize recovery.
struct WindowsDriverState final : ProcessRuntimeState {
  WindowsDriverState() : ProcessRuntimeState(Kind::WindowsDriverX64) {}
  enum class PoolAPI { ExAllocatePool, ExAllocatePoolWithTag, ExAllocatePool2 };
  struct PoolAllocation {
    /// Identity within this invocation's model. Freed addresses are not reused.
    uint64_t Address, RequestedSize;
    PoolAPI API;
    /// Zero-extended POOL_TYPE for legacy APIs, complete POOL_FLAGS otherwise.
    uint64_t TypeOrFlags;
    /// Zero only for the original untagged ExAllocatePool API.
    uint32_t Tag;
    /// Exactly RequestedSize owned bytes; absent when backing was not asked
    /// for. No padding, borrowed loader objects or inferred pointer fields.
    std::optional<std::vector<uint8_t>> Bytes;
  };
  std::vector<PoolAllocation> Pools;
  /// Includes unsupported resources/effects, even when no pool remains live.
  std::vector<std::string> Reasons;
  bool hasAdditionalDependencies() const override {
    return !Pools.empty() || !Reasons.empty();
  }
  std::vector<std::string> additionalDependencyReasons() const override {
    return Reasons;
  }
};
} // namespace neverd::emulation
#endif
