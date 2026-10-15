//===- KernelQueryBuffer.cpp - Unique kernel query RAM admission ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Unique kernel query RAM admission.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error uniqueKernelBuffer(GuestMemory &Memory, uint64_t Address,
                               uint32_t Size, llvm::StringRef API) {
  if (Address < profile::UserProbeLimit)
    return llvm::createStringError(API + " requires a kernel buffer");
  auto Space = Memory.addressSpace();
  if (!Space)
    return llvm::createStringError(API +
                                   " requires an address-space inventory");
  auto Buffer = Space->pinBacking(Address, Size);
  if (!Buffer)
    return Buffer.takeError();
  auto Mappings = Space->mappings();
  if (!Mappings)
    return Mappings.takeError();
  const auto CheckAlias = [&](uint64_t Begin, uint64_t Length) -> llvm::Error {
    if (!Length)
      return llvm::Error::success();
    auto Other = Space->pinBacking(Begin, Length);
    if (!Other)
      return Other.takeError();
    if (Buffer->overlaps(*Other))
      return llvm::createStringError(API + " does not support aliased buffers");
    return llvm::Error::success();
  };
  // Shared object guards use virtual identities. This bounded API excludes
  // alternate views, including no-access mappings, before reading or writing
  // their bytes. Compare physical identity, never address equality or values.
  // Adjacent mappings may be coalesced, so exclude only this exact VA span.
  for (const auto &Mapping : *Mappings) {
    if (Mapping.Device)
      continue;
    // A mapping may include the last address-space page. Its exclusive end
    // can be 2^64, so use lengths and offsets without forming that end.
    const uint64_t Prefix =
        Mapping.Address < Address
            ? std::min(Mapping.Size, Address - Mapping.Address)
            : 0;
    if (auto E = CheckAlias(Mapping.Address, Prefix))
      return E;
    const uint64_t Suffix =
        Mapping.Address < Address + Size
            ? std::min(Mapping.Size, Address + Size - Mapping.Address)
            : 0;
    if (Suffix < Mapping.Size)
      if (auto E = CheckAlias(Mapping.Address + Suffix, Mapping.Size - Suffix))
        return E;
  }
  return llvm::Error::success();
}
} // namespace

llvm::Error KernelModel::validateQueryBuffer(uint64_t Address, uint32_t Size,
                                             bool Writable,
                                             llvm::StringRef API) {
  if (!Address || !Size || Address > UINT64_MAX - Size)
    return llvm::createStringError(API + " has an invalid buffer extent");
  if (auto E = validateGuestAccess(Address, Size, Writable))
    return E;
  auto Accessible = Memory.canAccess(Address, Size, Writable ? Write : Read);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return llvm::createStringError(API + " requires accessible ordinary RAM");
  if (auto E = Memory.validateBacking(Address, Size))
    return E;
  return uniqueKernelBuffer(Memory, Address, Size, API);
}
} // namespace neverd::emulation
