//===- KernelRuntimeCalls.cpp - Unicode and debug calls -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::initializeUnicodeString(llvm::ArrayRef<uint64_t> A) {
  if (auto E = runtime::checkBufferRange(A[0], 16))
    return E;
  uint64_t Length = 0;
  if (A[1]) {
    if (auto E = runtime::checkBufferRange(A[1], MaxUnicodeBytes + 2))
      return E;
    while (Length <= MaxUnicodeBytes) {
      if (auto E = validateGuestAccess(A[1] + Length, 2, false))
        return E;
      auto Unit = Memory.readInteger(A[1] + Length, 2);
      if (!Unit)
        return Unit.takeError();
      if (!*Unit)
        break;
      Length += 2;
    }
    if (Length > MaxUnicodeBytes)
      return modelError("RtlInitUnicodeString source exceeds model capacity");
  }
  if (auto E = validateGuestAccess(A[0], 4, true))
    return E;
  if (auto E = validateGuestAccess(A[0] + 8, 8, true))
    return E;
  if (auto E = Memory.writeInteger(A[0], Length, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + 2, A[1] ? Length + 2 : 0, 2))
    return E;
  if (auto E = Memory.writeInteger(A[0] + 8, A[1], 8))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelModel::debugMessage(
    llvm::ArrayRef<uint64_t> A, unsigned FormatIndex,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument) {
  auto Text = runtime::formatDebugMessage(*this, Memory, A[FormatIndex],
                                          FormatIndex + 1, ReadArgument);
  if (!Text)
    return Text.takeError();
  // The observation profile enables all debugger component/level filters.
  Result.Messages.push_back(std::move(*Text));
  return StatusSuccess;
}

} // namespace neverd::emulation
