//===- ExecutionBackend.cpp - Normalized CPU fault names -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ExecutionDiagnostics.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd::emulation {
llvm::Expected<MemoryView> ExecutionBackend::pinBacking(uint64_t Address,
                                                        uint64_t Size) const {
  if (auto E = validateBacking(Address, Size))
    return E;
  return addressSpace()->pinBacking(Address, Size);
}
llvm::Error ExecutionBackend::validatePinned(const MemoryView &View,
                                             uint64_t Offset,
                                             uint64_t Size) const {
  if (auto E = validateBacking(0, 0))
    return E;
  return addressSpace()->validatePinned(View, Offset, Size);
}
llvm::Error ExecutionBackend::readPinned(const MemoryView &View,
                                         uint64_t Offset,
                                         llvm::MutableArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(0, 0))
    return E;
  return addressSpace()->readPinned(View, Offset, Bytes);
}
llvm::Error ExecutionBackend::writePinned(const MemoryView &View,
                                          uint64_t Offset,
                                          llvm::ArrayRef<uint8_t> Bytes) {
  if (auto E = validateBacking(0, 0))
    return E;
  return addressSpace()->writePinned(View, Offset, Bytes);
}
char BackendUnavailableError::ID;
void BackendUnavailableError::log(llvm::raw_ostream &OS) const { OS << Reason; }

char UnsupportedExecutionError::ID;
void UnsupportedExecutionError::log(llvm::raw_ostream &OS) const {
  OS << diagnostic::Instruction;
}

const char *backendFaultKindName(BackendFaultKind Kind) {
  switch (Kind) {
#define NEVERD_BACKEND_FAULT_KIND(Name, Spelling)                              \
  case BackendFaultKind::Name:                                                 \
    return Spelling;
#include "neverd/emulation/BackendFaults.def"
#undef NEVERD_BACKEND_FAULT_KIND
  }
  llvm_unreachable(diagnostic::UnknownFault);
}

const char *backendAccessKindName(BackendAccessKind Kind) {
  switch (Kind) {
#define NEVERD_BACKEND_ACCESS_KIND(Name, Spelling)                             \
  case BackendAccessKind::Name:                                                \
    return Spelling;
#include "neverd/emulation/BackendFaults.def"
#undef NEVERD_BACKEND_ACCESS_KIND
  }
  llvm_unreachable(diagnostic::UnknownAccess);
}

} // namespace neverd::emulation
