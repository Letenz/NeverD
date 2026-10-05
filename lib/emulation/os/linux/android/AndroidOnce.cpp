//===- AndroidOnce.cpp - Bionic once callbacks ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AndroidInternal.h"
#include "AndroidThreads.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::android_model {
BionicResult Bionic::once(const NativeCallEvent &Call) {
  const auto &A = Call.Arguments;
  if (A[0] % once_abi::ControlBytes)
    return failure(diagnostic::OnceControlAlignment);
  if (auto E = access(A[0], once_abi::ControlBytes, Read))
    return std::move(E);
  uint8_t Bytes[once_abi::ControlBytes];
  if (auto E = CPU.read(A[0], Bytes))
    return std::move(E);
  uint32_t State = llvm::support::endian::read32le(Bytes);
  if (State == once_abi::Complete)
    return std::optional<BionicValue>(uint64_t(0));
  if (State == once_abi::Running && Threads && Threads->enabled())
    return Threads->waitOnce(A[0]);
  if (State != once_abi::NotStarted ||
      (Threads && Threads->enabled() && Threads->onceActive(A[0]))) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = State == once_abi::Running
                            ? diagnostic::OnceInProgress
                            : diagnostic::OnceControlState;
    return std::optional<BionicValue>();
  }
  if (auto E = access(A[0], once_abi::ControlBytes, Write))
    return std::move(E);
  if (A[1] % 4)
    return failure(diagnostic::OnceInitializerAlignment);
  if (auto E = access(A[1], 4, Execute))
    return std::move(E);
  llvm::support::endian::write32le(Bytes, once_abi::Running);
  if (auto E = CPU.write(A[0], Bytes))
    return std::move(E);
  return std::optional<BionicValue>(
      GuestCallback{A[1], std::nullopt, OnceCallback{A[0]}});
}
llvm::Error Bionic::finishOnce(const OnceCallback &Callback) {
  if (auto E = access(Callback.Control, once_abi::ControlBytes, Write))
    return E;
  uint8_t Bytes[once_abi::ControlBytes];
  llvm::support::endian::write32le(Bytes, once_abi::Complete);
  if (auto E = CPU.write(Callback.Control, Bytes))
    return E;
  if (Threads && Threads->enabled())
    Threads->completeOnce(Callback.Control);
  return llvm::Error::success();
}

} // namespace neverd::emulation::android_model
