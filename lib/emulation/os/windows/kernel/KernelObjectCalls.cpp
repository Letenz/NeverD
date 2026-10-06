//===- KernelObjectCalls.cpp - Guest symbolic-link calls ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"

namespace neverd::emulation {
llvm::Expected<uint64_t>
KernelModel::deleteSymbolicLinkFromGuest(llvm::ArrayRef<uint64_t> A) {
  auto ObjectName = readObjectName(A[0]);
  if (!ObjectName)
    return ObjectName.takeError();

  auto Status = deleteSymbolicLink(*ObjectName);
  if (!Status)
    return Status.takeError();
  return *Status;
}

llvm::Expected<uint64_t>
KernelModel::createSymbolicLinkFromGuest(llvm::ArrayRef<uint64_t> A) {
  auto ObjectName = readObjectName(A[0]);
  if (!ObjectName)
    return ObjectName.takeError();

  auto Target = readObjectName(A[1]);
  if (!Target)
    return Target.takeError();
  auto Status = createSymbolicLink(*ObjectName, *Target);
  if (!Status)
    return Status.takeError();
  return *Status;
}

} // namespace neverd::emulation
