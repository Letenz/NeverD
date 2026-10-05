//===- LinuxUserMemory.cpp - Shared fixed-size user copy policy ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxUserMemory.h"

namespace neverd::emulation::linux_model {
llvm::Expected<UserWriteResult> writeUserMemory(ExecutionBackend &CPU,
                                                const MemoryLayout &Layout,
                                                uint64_t Address,
                                                llvm::ArrayRef<uint8_t> Bytes) {
  if (Address > Layout.UserLimit || Bytes.size() > Layout.UserLimit - Address)
    return UserWriteResult::BadAddress;
  if (Bytes.empty())
    return UserWriteResult::Stored;
  auto Writable = CPU.canAccess(Address, Bytes.size(), Write | UserAccessible);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable) {
    for (size_t I = 0; I < Bytes.size(); ++I) {
      auto Part = CPU.canAccess(Address + I, 1, Write | UserAccessible);
      if (!Part)
        return Part.takeError();
      if (*Part)
        return UserWriteResult::MixedAccess;
    }
    return UserWriteResult::BadAddress;
  }
  if (auto E = CPU.write(Address, Bytes))
    return std::move(E);
  return UserWriteResult::Stored;
}
} // namespace neverd::emulation::linux_model
