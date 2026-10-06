//===- KernelIRPCalls.cpp - IRP ownership and completion ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::setCancelRoutine(llvm::ArrayRef<uint64_t> A) {
  const auto *Request = requestForIRP(A[0]);
  if (!Request || Request->Completed ||
      (Framework && Framework->ownsRequestIRP(A[0])))
    return modelError("IoSetCancelRoutine requires a live WDM-owned IRP");
  auto Previous = Memory.readInteger(A[0] + IRPCancelRoutineOffset, 8);
  if (!Previous)
    return Previous.takeError();
  if (auto E = Memory.writeInteger(A[0] + IRPCancelRoutineOffset, A[1], 8))
    return E;
  return *Previous;
}

llvm::Expected<uint64_t>
KernelModel::callPowerDriver(llvm::ArrayRef<uint64_t> A) {
  const auto *Request = requestForIRP(A[1]);
  if (!Request || Request->Kind != DriverRequestKind::Power)
    return modelError("PoCallDriver requires an owned power IRP");
  return callDriver(A[0], A[1]);
}

llvm::Expected<uint64_t>
KernelModel::completeDriverRequest(llvm::ArrayRef<uint64_t> A) {
  if (FrameworkUsbIdleRequests.contains(A[0]))
    return modelError("framework owns its USB idle completion");
  if (Framework && Framework->ownsRequestIRP(A[0]))
    return modelError(
        "framework-owned requests must complete through WdfRequestComplete");
  if (auto E = completeRequest(A[0], static_cast<uint8_t>(A[1])))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::currentDriverRequestStack(llvm::ArrayRef<uint64_t> A) {
  const auto *Request = requestForIRP(A[0]);
  if (!Request || Request->Completed)
    return modelError(
        "IoGetCurrentIrpStackLocation requires the live request IRP");
  return currentRequestStack(A[0]);
}

} // namespace neverd::emulation
