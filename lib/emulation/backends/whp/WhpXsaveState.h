//===- WhpXsaveState.h - Complete bounded x64 FP/SSE transport ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_XSAVESTATE_H
#define NEVERD_EMULATION_WHP_XSAVESTATE_H

#include "../../arch/x86_64/X64Machine.h"
#include "WhpPartition.h"

#include "llvm/Support/Memory.h"

namespace neverd::emulation {
/// WHP's legacy register interface does not preserve the complete physical
/// x87 state. Transfer one XSAVE packet and use the authoritative ISA codec.
class WhpXsaveState {
public:
  WhpXsaveState() = default;
  WhpXsaveState(const WhpXsaveState &) = delete;
  WhpXsaveState &operator=(const WhpXsaveState &) = delete;
  ~WhpXsaveState() { (void)llvm::sys::Memory::releaseMappedMemory(Buffer); }
  llvm::Error initialize(WhpAPI &API, WHV_PARTITION_HANDLE Partition) {
    if (Buffer.base())
      return diagnostic::error(diagnostic::WhpState);
    Modern = API.WHvGetVirtualProcessorState && API.WHvSetVirtualProcessorState;
    if (!Modern && !(API.WHvGetVirtualProcessorXsaveState &&
                     API.WHvSetVirtualProcessorXsaveState))
      return diagnostic::unavailable(diagnostic::WhpCapability,
                                     BackendAvailability::HostAPI);
    UINT32 Required = 0;
    const HRESULT Status = get(API, Partition, nullptr, 0, Required);
    if (uint32_t(Status) != InsufficientBuffer)
      return whpError(diagnostic::WhpState, Status);
    if (Required < x64::fp::XsaveBytes || Required > x64::fp::MaxXsaveBytes)
      return diagnostic::error(diagnostic::FPState);
    std::error_code EC;
    Buffer = llvm::sys::Memory::allocateMappedMemory(
        Required, nullptr,
        llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
    if (EC)
      return llvm::errorCodeToError(EC);
    Size = Required;
    return llvm::Error::success();
  }
  llvm::Error install(WhpAPI &API, WHV_PARTITION_HANDLE Partition,
                      const X64MachineState &State) {
    if (auto E = encodeX64XsaveState(State, bytes(), true))
      return E;
    const auto Status =
        Modern ? API.WHvSetVirtualProcessorState(
                     Partition, 0, WHvVirtualProcessorStateTypeXsaveState,
                     Buffer.base(), Size)
               : API.WHvSetVirtualProcessorXsaveState(Partition, 0,
                                                      Buffer.base(), Size);
    return FAILED(Status) ? whpError(diagnostic::WhpState, Status)
                          : llvm::Error::success();
  }
  llvm::Error capture(WhpAPI &API, WHV_PARTITION_HANDLE Partition,
                      X64MachineState &State) {
    UINT32 Written = 0;
    const auto Status = get(API, Partition, Buffer.base(), Size, Written);
    if (FAILED(Status))
      return whpError(diagnostic::WhpState, Status);
    if (Written < x64::fp::XsaveBytes || Written > Size)
      return diagnostic::error(diagnostic::FPState);
    return decodeX64XsaveState(State, bytes().take_front(Written));
  }

private:
  llvm::MutableArrayRef<uint8_t> bytes() {
    return {static_cast<uint8_t *>(Buffer.base()), Size};
  }
  HRESULT get(WhpAPI &API, WHV_PARTITION_HANDLE Partition, void *Bytes,
              UINT32 Capacity, UINT32 &Written) {
    return Modern ? API.WHvGetVirtualProcessorState(
                        Partition, 0, WHvVirtualProcessorStateTypeXsaveState,
                        Bytes, Capacity, &Written)
                  : API.WHvGetVirtualProcessorXsaveState(Partition, 0, Bytes,
                                                         Capacity, &Written);
  }
  llvm::sys::MemoryBlock Buffer;
  UINT32 Size = 0;
  bool Modern = false;
};
} // namespace neverd::emulation
#endif
