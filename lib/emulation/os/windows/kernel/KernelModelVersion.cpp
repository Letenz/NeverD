//===- KernelModelVersion.cpp - Bounded base version output
//-----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded version output with explicit RAM and alias admission.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "KernelVersionQuery.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
llvm::Error uniqueKernelBuffer(GuestMemory &Memory, uint64_t Address,
                               uint32_t Size) {
  if (Address < profile::UserProbeLimit)
    return llvm::createStringError("RtlGetVersion requires a kernel buffer");
  auto Space = Memory.addressSpace();
  if (!Space)
    return llvm::createStringError(
        "RtlGetVersion requires an address-space inventory");
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
      return llvm::createStringError(
          "RtlGetVersion does not support aliased buffers");
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

llvm::Expected<uint64_t> KernelModel::queryVersion(uint64_t Address) {
  using namespace kernel_version_query;
  if (Contract != ExecutionContract::CheckedX64)
    return llvm::createStringError(
        "RtlGetVersion requires checked-x64 execution");
  if (!VersionQuery)
    return llvm::createStringError(
        "RtlGetVersion version_query is not declared");
  if (!Address || Address > UINT64_MAX - FieldsEnd)
    return llvm::createStringError(
        "RtlGetVersion has an invalid buffer extent");
  const auto Check = [&](uint64_t Begin, uint32_t Size,
                         bool Writable) -> llvm::Error {
    if (auto E = validateGuestAccess(Begin, Size, Writable))
      return E;
    auto Accessible = Memory.canAccess(Begin, Size, Writable ? Write : Read);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return llvm::createStringError(
          "RtlGetVersion requires accessible ordinary RAM");
    if (auto E = Memory.validateBacking(Begin, Size))
      return E;
    return uniqueKernelBuffer(Memory, Begin, Size);
  };
  if (auto E = Check(Address, 4, false))
    return E;
  auto Size = Memory.readInteger(Address, 4);
  if (!Size)
    return Size.takeError();
  if (*Size != BaseSize && !(*Size == 0 && VersionQuery->AllowZeroSize))
    return llvm::createStringError("RtlGetVersion record size is unsupported");
  const uint32_t Extent = *Size == BaseSize ? BaseSize : FieldsEnd;
  if (Address > UINT64_MAX - Extent)
    return llvm::createStringError(
        "RtlGetVersion has an invalid buffer extent");
  // Validate every read and output byte before committing any write. An
  // opt-in zero-size call has no CSD access, even when its page ends at +20.
  if (auto E = Check(Address, Extent, false))
    return E;
  if (auto E = Check(Address + 4, FieldsEnd - 4, true))
    return E;
  if (*Size == BaseSize) {
    std::array<uint8_t, BaseSize - FieldsEnd> CSD;
    if (auto E = Memory.read(Address + FieldsEnd, CSD))
      return E;
    if (std::any_of(CSD.begin(), CSD.end(), [](uint8_t B) { return B != 0; }))
      return llvm::createStringError(
          "RtlGetVersion base record requires a zeroed CSD array");
  }
  std::array<uint8_t, FieldsEnd - 4> Fields{};
  llvm::support::endian::write32le(Fields.data(), 10);
  llvm::support::endian::write32le(Fields.data() + 4, 0);
  llvm::support::endian::write32le(Fields.data() + 8,
                                   *VersionQuery->BuildNumber);
  llvm::support::endian::write32le(Fields.data() + 12, 2);
  if (auto E = Memory.write(Address + 4, Fields))
    return E;
  return uint64_t(0);
}
} // namespace neverd::emulation
