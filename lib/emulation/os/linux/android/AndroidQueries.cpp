//===- AndroidQueries.cpp - Bionic platform queries ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../kernel/LinuxTime.h"
#include "AndroidInternal.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
llvm::Error Bionic::setErrno(uint32_t Value) {
  uint8_t Bytes[4];
  llvm::support::endian::write32le(Bytes, Value);
  return CPU.write(tlsAddress() + ErrnoAddress - TLSAddress, Bytes);
}
BionicResult Bionic::getErrno(const NativeCallEvent &) {
  return value(tlsAddress() + ErrnoAddress - TLSAddress);
}

BionicResult Bionic::getPageSize(const NativeCallEvent &) {
  return value(PageSize);
}

BionicResult Bionic::getAPILevel(const NativeCallEvent &) { return value(28); }

BionicResult Bionic::queryConfiguration(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  // sysconf takes int, independent of unspecified upper GP argument bits.
  switch (static_cast<SysconfName>(static_cast<uint32_t>(A[0]))) {
  case SysconfName::PageSize:
  case SysconfName::PageSizeAlias:
    return value(PageSize);
  }
  Result.Stop = ProcessStopReason::UnsupportedService;
  Result.Diagnostic = diagnostic::SysconfSelector;
  return std::optional<BionicValue>();
}

BionicResult Bionic::getTime(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Now = Kernel.clock().read(linux_model::ClockRealtime, Result);
  if (!Now)
    return std::optional<BionicValue>();
  // API 28's fallback obtains a timeval, then stores through the caller's
  // time_t*. This is a user-space store, not the x64 time syscall's EFAULT.
  if (A[0]) {
    if (auto E = access(A[0], 8, Write))
      return std::move(E);
    uint8_t Bytes[8];
    llvm::support::endian::write64le(Bytes,
                                     static_cast<uint64_t>(Now->Seconds));
    if (auto E = CPU.write(A[0], Bytes))
      return std::move(E);
  }
  // A valid negative timestamp is not a Linux errno return.
  return value(static_cast<uint64_t>(Now->Seconds));
}

BionicResult Bionic::getProperty(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  auto Key = string(A[0]);
  if (!Key)
    return Key.takeError();
  auto I = Options.Android->Properties.find(*Key);
  std::string Text = I == Options.Android->Properties.end() ? "" : I->second;
  if (auto E = access(A[1], Text.size() + 1, Write))
    return std::move(E);
  std::vector<uint8_t> Bytes(Text.begin(), Text.end());
  Bytes.push_back(0);
  if (auto E = CPU.write(A[1], Bytes))
    return std::move(E);
  return value(Text.size());
}

} // namespace neverd::emulation::android_model
