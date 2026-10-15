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

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
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
    return validateQueryBuffer(Begin, Size, Writable, "RtlGetVersion");
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
