//===- DarwinUserMemory.cpp - Shared fixed-copy permission checks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinUserMemory.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
llvm::Expected<std::optional<ServiceResult>>
copyUserMemory(GuestMemory &Memory, uint64_t Address,
               llvm::ArrayRef<uint8_t> Bytes, const char *PartialDiagnostic,
               ProcessResult &Result) {
  auto returned = [](uint64_t Value, bool Error = false) {
    return std::optional<ServiceResult>({Value, Error});
  };
  auto unsupported = [](ProcessResult &Result, const char *Reason) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Reason;
    return std::optional<ServiceResult>();
  };
  if (Address >= UserLimit)
    return returned(BadAddress, true);
  // Copyout failures do not have a portable partial-effects contract. Reject
  // a writable prefix before publishing any bytes or advancing a cursor.
  uint64_t Checked = 0;
  while (Checked < Bytes.size()) {
    const uint64_t Start = Address + Checked;
    const uint64_t Size = std::min(Bytes.size() - Checked, 4096 - Start % 4096);
    if (Start >= UserLimit)
      return Checked ? unsupported(Result, PartialDiagnostic)
                     : returned(BadAddress, true);
    auto Access = Memory.canAccess(Start, Size, Write | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      return Checked ? unsupported(Result, PartialDiagnostic)
                     : returned(BadAddress, true);
    Checked += Size;
  }
  if (auto E = Memory.write(Address, Bytes))
    return std::move(E);
  return returned(0);
}
} // namespace neverd::emulation::darwin_model
