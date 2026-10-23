//===- X86SystemStateNative.h - Native system APIs ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native API selection for bounded system effects. The assembly
/// contract remains the authority for lifted operands and legacy SDK output.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86SYSTEMSTATENATIVE_H
#define NEVERD_BACKEND_LLVM_X86SYSTEMSTATENATIVE_H

#include "neverd/backend/llvm/X86SystemStateAsm.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

inline constexpr Intrinsic X86SystemStateNativeInventory[] = {
    Intrinsic::X86ClearTaskSwitched,   Intrinsic::X86WriteMsrNonSerializing,
    Intrinsic::X86ReadMsrList,         Intrinsic::X86WriteMsrList,
    Intrinsic::X86LoadKernelGs,        Intrinsic::X86LoadKernelGsMemory,
    Intrinsic::X86InvalidateBroadcast, Intrinsic::X86TlbSync,
    Intrinsic::X86LoadAccessRights,    Intrinsic::X86LoadAccessRightsMemory,
    Intrinsic::X86LoadSegmentLimit,    Intrinsic::X86LoadSegmentLimitMemory};

struct X86SystemStateNative {
  X86SystemStateAsm Assembly;
  std::string LLVMName;
  std::string Builtin;
  std::vector<uint16_t> InputBytes;
  bool PackMsr = false;
  bool DescriptorQuery = false;
};

inline std::optional<X86SystemStateNative>
x86SystemStateNative(Intrinsic Id, Arch TargetArch, uint16_t Width,
                     NdMemoryAddressSpace Space) {
  // The current memory APIs own flat linear pointers. A segmented selector
  // needs a separate explicit-address intrinsic; never erase its segment.
  if (Space != NdMemoryAddressSpace::Default)
    return std::nullopt;
  bool Known = false;
  for (const auto Candidate : X86SystemStateNativeInventory)
    Known |= Id == Candidate;
  if (!Known)
    return std::nullopt;
  const auto Assembly = x86SystemStateAsm(Id, TargetArch, Width, Space);
  if (!Assembly)
    return std::nullopt;
  X86SystemStateNative Plan;
  Plan.Assembly = *Assembly;
  Plan.PackMsr = Id == Intrinsic::X86WriteMsrNonSerializing;
  const auto C = x86SystemStateContract(Id);
  Plan.DescriptorQuery = C.Kind == X86SystemStateKind::AccessRights;
  Plan.InputBytes =
      Plan.PackMsr ? std::vector<uint16_t>{4, 8} : Assembly->InputBytes;
  std::string Name = C.Mnemonic;
  if (Plan.DescriptorQuery) {
    Name += C.MemoryArgument == 0 ? ".mem" : ".reg";
    Name += std::to_string(8 * Width);
    Plan.InputBytes.push_back(Width);
  } else if (Id == Intrinsic::X86LoadKernelGs)
    Name += ".reg";
  else if (Id == Intrinsic::X86LoadKernelGsMemory)
    Name += ".mem";
  else if (Id == Intrinsic::X86InvalidateBroadcast && TargetArch == Arch::X64 &&
           Width == 4)
    Name += ".addr32";
  Plan.Builtin = "__builtin_ia32_" + Name;
  for (char &Character : Plan.Builtin)
    if (Character == '.')
      Character = '_';
  if (Id == Intrinsic::X86InvalidateBroadcast && TargetArch == Arch::X86)
    Name += "32";
  Plan.LLVMName = "llvm.x86." + Name;
  return Plan;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86SYSTEMSTATENATIVE_H
