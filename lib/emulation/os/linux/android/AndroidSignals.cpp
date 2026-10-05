//===- AndroidSignals.cpp - Bionic LP64 signal action translation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
namespace {
// API 28 Bionic reserves real-time signals 32 through 35 for its runtime.
constexpr uint64_t ReservedSignals = uint64_t(0xf) << 31;
// The four-byte flags field precedes four bytes of padding in the LP64 ABI.
constexpr unsigned ActionSize = 32;
} // namespace

BionicResult Bionic::signalAction(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  LinuxSignalAction NewAction;
  if (A[1]) {
    if (auto E = access(A[1], ActionSize, Read))
      return std::move(E);
    uint8_t Bytes[ActionSize];
    if (auto E = CPU.read(A[1], Bytes))
      return std::move(E);
    NewAction.Flags =
        uint64_t(int64_t(int32_t(llvm::support::endian::read32le(Bytes))));
    NewAction.Handler = llvm::support::endian::read64le(Bytes + 8);
    NewAction.Mask =
        llvm::support::endian::read64le(Bytes + 16) & ~ReservedSignals;
    NewAction.Restorer = llvm::support::endian::read64le(Bytes + 24);
  }
  auto Applied = Kernel.signalAction(int32_t(uint32_t(A[0])),
                                     A[1] ? &NewAction : nullptr, A[2] != 0);
  if (!Applied)
    return std::optional<BionicValue>();
  if (Applied->Status) {
    if (auto E = setErrno(uint64_t(0) - Applied->Status))
      return std::move(E);
    // API 28's LP64 wrapper copies an uninitialized kernel temporary after
    // failure. Its bytes cannot be replaced with zeros or the previous action.
    if (A[2]) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::SignalIndeterminateOutput;
      return std::optional<BionicValue>();
    }
    return value(UINT64_MAX);
  }
  if (A[2]) {
    const auto &Old = Applied->Previous;
    struct Field {
      unsigned Offset, Size;
      uint64_t Value;
    };
    const Field Fields[] = {{0, 4, Old.Flags},
                            {8, 8, Old.Handler},
                            {16, 8, Old.Mask},
                            {24, 8, Old.Restorer}};
    for (const auto &F : Fields) {
      if (A[2] > UINT64_MAX - F.Offset)
        return failure(diagnostic::GuestPointer);
      const uint64_t Address = A[2] + F.Offset;
      if (auto E = access(Address, F.Size, Write))
        return std::move(E);
      uint8_t Bytes[8];
      llvm::support::endian::write64le(Bytes, F.Value);
      if (auto E = CPU.write(Address, llvm::ArrayRef(Bytes).take_front(F.Size)))
        return std::move(E);
    }
  }
  return value(0);
}
} // namespace neverd::emulation::android_model
