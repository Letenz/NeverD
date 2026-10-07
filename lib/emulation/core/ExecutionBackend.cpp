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
llvm::Error ExecutionBackend::run(uint64_t PC, uint64_t Timeout) {
  auto Exit = runUntilExit(PC, Timeout);
  if (!Exit)
    return Exit.takeError();
  if (Exit->Kind == ExecutionExitKind::UnsupportedOperation)
    return llvm::make_error<UnsupportedExecutionError>();
  if (Exit->Kind == ExecutionExitKind::ServiceRequest)
    return diagnostic::error(diagnostic::PendingService);
  if (!Exit->Diagnostic.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   Exit->Diagnostic);
  return llvm::Error::success();
}

llvm::Expected<ExecutionExit> ExecutionBackend::runUntilExit(uint64_t,
                                                             uint64_t) {
  return diagnostic::error(diagnostic::TypedExecutionUnsupported);
}

llvm::Error ExecutionBackend::setMemoryWriteWatches(
    const std::vector<MemoryWriteWatch> &Watches) {
  if (!Watches.empty())
    return diagnostic::error(diagnostic::WriteWatchesUnsupported);
  return llvm::Error::success();
}

llvm::Expected<uint32_t> ExecutionBackend::instructionSize(uint64_t) {
  return diagnostic::error(diagnostic::InstructionInspectionUnsupported);
}

const char *executionExitKindName(ExecutionExitKind Kind) {
  switch (Kind) {
#define NEVERD_EXECUTION_EXIT(Name, Text)                                      \
  case ExecutionExitKind::Name:                                                \
    return Text;
#include "neverd/emulation/ExecutionExit.def"
#undef NEVERD_EXECUTION_EXIT
  }
  llvm_unreachable(diagnostic::UnknownExit);
}

const char *serviceRequestKindName(ServiceRequestKind Kind) {
  switch (Kind) {
#define NEVERD_SERVICE_REQUEST(Name, Text)                                     \
  case ServiceRequestKind::Name:                                               \
    return Text;
#include "neverd/emulation/ServiceRequest.def"
#undef NEVERD_SERVICE_REQUEST
  }
  llvm_unreachable(diagnostic::UnknownService);
}

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

const char *backendFaultCauseName(BackendFaultCause Cause) {
  switch (Cause) {
#define NEVERD_BACKEND_FAULT_CAUSE(Name, Spelling)                             \
  case BackendFaultCause::Name:                                                \
    return Spelling;
#include "neverd/emulation/BackendFaults.def"
#undef NEVERD_BACKEND_FAULT_CAUSE
  }
  llvm_unreachable(diagnostic::UnknownFault);
}

} // namespace neverd::emulation
