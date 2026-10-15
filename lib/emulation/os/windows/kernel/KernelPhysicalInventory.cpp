//===- KernelPhysicalInventory.cpp - Explicit simulated inventory ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit simulated inventory.
///
//===----------------------------------------------------------------------===//
#include "KernelPhysicalInventory.h"

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace kernel_physical_inventory {
llvm::Error validate(const DriverPhysicalMemoryInventory &Inventory) {
  if (Inventory.Version != Version)
    return llvm::createStringError(
        "driver physical_memory_inventory: unsupported version");
  if (Inventory.Source !=
      DriverPhysicalMemoryInventory::SourceKind::ModelIdentityWindow)
    return llvm::createStringError(
        "driver physical_memory_inventory: source must select "
        "model-identity-window");
  if (Inventory.Evidence.empty() ||
      Inventory.Evidence.size() > MaxEvidenceSize ||
      !std::all_of(Inventory.Evidence.begin(), Inventory.Evidence.end(),
                   [](unsigned char C) { return C >= ' ' && C <= '~'; }))
    return llvm::createStringError(
        "driver physical_memory_inventory: evidence must be 1..256 "
        "printable ASCII bytes");
  return llvm::Error::success();
}
} // namespace kernel_physical_inventory

llvm::Expected<uint64_t> KernelModel::queryPhysicalMemoryRanges() {
  if (!PhysicalMemoryInventory)
    return llvm::createStringError(
        "MmGetPhysicalMemoryRanges physical_memory_inventory is not declared");
  // The declared environment selects this eligible RAM domain explicitly.
  // Enumerating it neither creates backing nor grants access to unknown pages.
  const auto [Base, Size] = Physical.identityWindow();
  std::array<uint8_t, 32> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), Base);
  llvm::support::endian::write64le(Bytes.data() + 8, Size);
  auto Buffer = allocatePool({windows::PoolNX, Bytes.size()}, false,
                             PoolSource::KernelResult);
  if (!Buffer || !*Buffer)
    return Buffer;
  if (auto E = Memory.write(*Buffer, Bytes))
    return E;
  return *Buffer;
}
} // namespace neverd::emulation
