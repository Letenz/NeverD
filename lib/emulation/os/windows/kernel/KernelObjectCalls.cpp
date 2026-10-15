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
  // NtCreateSymbolicLinkObject captures MaximumLength bytes, including any
  // caller-supplied terminator or padding. Queries with ReturnedLength expose
  // that complete captured buffer, not an invented null terminator.
  auto Maximum = Memory.readInteger(A[1] + 2, 2);
  if (!Maximum)
    return Maximum.takeError();
  auto Buffer = Memory.readInteger(A[1] + 8, 8);
  if (!Buffer)
    return Buffer.takeError();
  const uint32_t Size = uint16_t(*Maximum) & ~1u;
  if (auto E = validateGuestAccess(*Buffer, Size, false))
    return E;
  std::vector<uint8_t> Bytes(Size);
  if (auto E = Memory.read(*Buffer, Bytes))
    return E;
  auto Status = createSymbolicLink(*ObjectName, *Target);
  if (!Status)
    return Status.takeError();
  if (!*Status) {
    auto Key = linkKey(*ObjectName);
    if (!Key)
      return Key.takeError();
    SymbolicLinks.at(findSymbolicLink(*Key)).TargetBytes = std::move(Bytes);
  }
  return *Status;
}

} // namespace neverd::emulation
