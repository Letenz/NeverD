//===- DarwinUserMemory.cpp - Shared fixed-copy permission checks ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinUserMemory.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
llvm::Expected<uint64_t> userMemoryPrefix(GuestMemory &Memory, uint64_t Address,
                                          uint64_t Size, unsigned Permissions) {
  uint64_t Checked = 0;
  if (Address >= UserLimit)
    return Checked;
  while (Checked < Size) {
    const uint64_t Start = Address + Checked;
    const uint64_t Chunk = std::min(Size - Checked, 4096 - Start % 4096);
    if (Start >= UserLimit)
      break;
    auto Access = Memory.canAccess(Start, Chunk, Permissions | UserAccessible);
    if (!Access)
      return Access.takeError();
    if (!*Access)
      break;
    Checked += Chunk;
  }
  return Checked;
}
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
  auto Prefix = userMemoryPrefix(Memory, Address, Bytes.size(), Write);
  if (!Prefix)
    return Prefix.takeError();
  if (*Prefix != Bytes.size())
    return *Prefix ? unsupported(Result, PartialDiagnostic)
                   : returned(BadAddress, true);
  if (auto E = Memory.write(Address, Bytes))
    return std::move(E);
  return returned(0);
}
} // namespace neverd::emulation::darwin_model
