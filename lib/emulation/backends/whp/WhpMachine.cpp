//===- WhpMachine.cpp - Windows x64 execution----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../arch/x86_64/X64Exception.h"
#include "../../arch/x86_64/X64Machine.h"
#include "../../arch/x86_64/X64MachineProbe.h"
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../MachineFactories.h"
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) &&             \
    defined(NEVERD_EMULATION_WHP)
#include "WhpResourceCache.h"
#include "WhpX64Processor.h"

#include <windows.h>

namespace neverd::emulation {
namespace {
llvm::Expected<std::unique_ptr<WhpVirtualProcessor>>
createWhpX64Processor(MemoryProjection &Memory);
class WhpMachine final : public X64Machine {
public:
  // Private startup execution discovers and verifies this mask before return.
  uint32_t MXCSRMask = x64::fp::ArchitecturalMXCSRMask;
  uint32_t mxcsrMask() const override { return MXCSRMask; }
  X64BranchModel BranchModel = X64BranchModel::Intel;
  X64BranchModel branchModel() const override { return BranchModel; }
  explicit WhpMachine(MemoryProjection &Memory)
      : Binding([&Memory] { return createWhpX64Processor(Memory); },
                Memory.parallelEnabled()) {}
  llvm::Error initialize() {
    auto Active = Binding.acquire(
        {std::chrono::steady_clock::now() +
         std::chrono::microseconds(x64::probe::TimeoutMicroseconds)});
    return Active ? llvm::Error::success() : Active.takeError();
  }
  llvm::Error step(X64MachineState &State, uint64_t Root,
                   MachineRunControl Control) override {
    Control = Control.forNativeStep();
    if (auto E = validateX64FPState(State.FP))
      return E;
    auto Active = Binding.acquire(Control);
    if (!Active)
      return Active.takeError();
    return static_cast<WhpX64Processor &>(**Active).step(State, Root, Control,
                                                         MXCSRMask);
  }

private:
  WhpResourceBinding<WhpVirtualProcessor> Binding;
};
llvm::Error configureWhpX64Processor(WhpAPI &API,
                                     WHV_PARTITION_HANDLE &Partition) {
  WHV_CAPABILITY C{};
  if (const auto Status = API.WHvGetCapability(
          WHvCapabilityCodeHypervisorPresent, &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpCapability, Status,
                          whp::operation::WHvGetCapability);
  if (!C.HypervisorPresent)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (const auto Status = API.WHvGetCapability(WHvCapabilityCodeExtendedVmExits,
                                               &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpCapability, Status,
                          whp::operation::WHvGetCapability);
  if (!C.ExtendedVmExits.ExceptionExit)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (const auto Status = API.WHvGetCapability(
          WHvCapabilityCodeExceptionExitBitmap, &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpCapability, Status,
                          whp::operation::WHvGetCapability);
  if ((C.ExceptionExitBitmap & x64::ExceptionExitBitmap) !=
      x64::ExceptionExitBitmap)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (const auto Status = API.WHvGetCapability(
          WHvCapabilityCodeProcessorXsaveFeatures, &C, sizeof(C), nullptr);
      FAILED(Status))
    return whpUnavailable(diagnostic::WhpCapability, Status,
                          whp::operation::WHvGetCapability);
  if (!C.ProcessorXsaveFeatures.XsaveSupport)
    return diagnostic::unavailable(diagnostic::WhpCapability,
                                   BackendAvailability::MissingCapability);
  if (const auto Status = API.WHvCreatePartition(&Partition); FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvCreatePartition);
  WHV_PARTITION_PROPERTY P{};
  P.ProcessorCount = ProcessorCount;
  if (const auto Status = API.WHvSetPartitionProperty(
          Partition, WHvPartitionPropertyCodeProcessorCount, &P, sizeof(P));
      FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetPartitionProperty);
  P = {};
  P.ExtendedVmExits.ExceptionExit = 1;
  if (const auto Status = API.WHvSetPartitionProperty(
          Partition, WHvPartitionPropertyCodeExtendedVmExits, &P, sizeof(P));
      FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetPartitionProperty);
  // Keep the host's coherent default XSAVE feature set. Enabling only XSAVE
  // while clearing its dependent feature bits can make setup fail with
  // ERROR_HV_INVALID_PARAMETER. Guest XCR0 still admits only x87 and SSE.
  P = {};
  P.ExceptionExitBitmap = x64::ExceptionExitBitmap;
  if (const auto Status = API.WHvSetPartitionProperty(
          Partition, WHvPartitionPropertyCodeExceptionExitBitmap, &P,
          sizeof(P));
      FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetPartitionProperty);
  if (const auto Status = API.WHvSetupPartition(Partition); FAILED(Status))
    return whpError(diagnostic::WhpCreate, Status,
                    whp::operation::WHvSetupPartition);
  return llvm::Error::success();
}
llvm::Expected<std::unique_ptr<WhpVirtualProcessor>>
createWhpX64Processor(MemoryProjection &Memory) {
  auto M = std::make_unique<WhpX64Processor>();
  if (auto E = M->attach(Memory, configureWhpX64Processor))
    return E;
  if (auto E = M->Xsave.initialize(M->API, M->Partition, M->ProcessorIndex))
    return E;
  if (auto E = M->initializeRunControl())
    return E;
  return std::unique_ptr<WhpVirtualProcessor>(std::move(M));
}
} // namespace
llvm::Expected<std::unique_ptr<X64Machine>>
createWhpMachine(MemoryProjection &Memory) {
  auto M = std::make_unique<WhpMachine>(Memory);
  if (auto E = M->initialize())
    return E;
  if (auto E =
          verifyX64Machine(*M, Memory, &M->MXCSRMask, true, &M->BranchModel))
    return E;
  return std::unique_ptr<X64Machine>(std::move(M));
}
} // namespace neverd::emulation
#else

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<X64Machine>>
createWhpMachine(MemoryProjection &) {
  return diagnostic::unavailable(diagnostic::Unavailable);
}
} // namespace neverd::emulation
#endif
