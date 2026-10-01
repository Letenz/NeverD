//===- WhpXsaveState.h - Complete bounded x64 FP/SSE transport ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_XSAVESTATE_H
#define NEVERD_EMULATION_WHP_XSAVESTATE_H

#include "../../arch/x86_64/X64Machine.h"
#include "WhpPartition.h"

#include "llvm/Support/Endian.h"
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
    if (!API.WHvGetPartitionProperty ||
        (!Modern && !(API.WHvGetVirtualProcessorXsaveState &&
                      API.WHvSetVirtualProcessorXsaveState)))
      return diagnostic::unavailable(diagnostic::WhpCapability,
                                     BackendAvailability::HostAPI);
    // Query the effective partition, not just host capabilities. Preserve its
    // default feature dependencies; the ISA codec rejects active extensions
    // outside the checked FP/SSE contract without narrowing the host mask.
    WHV_PROCESSOR_XSAVE_FEATURES Features{};
    UINT32 FeatureBytes = 0;
    const auto FeatureStatus = API.WHvGetPartitionProperty(
        Partition, WHvPartitionPropertyCodeProcessorXsaveFeatures, &Features,
        sizeof(Features), &FeatureBytes);
    if (FAILED(FeatureStatus))
      return whpError(diagnostic::WhpState, FeatureStatus,
                      whp::operation::WHvGetPartitionProperty);
    if (FeatureBytes != sizeof(Features))
      return diagnostic::error(diagnostic::WhpState);
    if (!Features.XsaveSupport)
      return diagnostic::unavailable(diagnostic::WhpCapability,
                                     BackendAvailability::MissingCapability);
    UINT32 Required = 0;
    const HRESULT Status = get(API, Partition, nullptr, 0, Required);
    if (uint32_t(Status) != InsufficientBuffer)
      return whpError(diagnostic::WhpState, Status, getOperation());
    if (Required < x64::fp::XsaveBytes || Required > x64::fp::MaxXsaveBytes)
      return sizeError(Required, 0);
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
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     llvm::formatv(XsaveEncodeFailure,
                                                   llvm::toString(std::move(E)),
                                                   setOperation(), Size)
                                         .str());
    const auto Status =
        Modern ? API.WHvSetVirtualProcessorState(
                     Partition, 0, WHvVirtualProcessorStateTypeXsaveState,
                     Buffer.base(), Size)
               : API.WHvSetVirtualProcessorXsaveState(Partition, 0,
                                                      Buffer.base(), Size);
    return FAILED(Status)
               ? whpError(diagnostic::WhpState, Status, setOperation())
               : llvm::Error::success();
  }
  llvm::Error capture(WhpAPI &API, WHV_PARTITION_HANDLE Partition,
                      X64MachineState &State) {
    UINT32 Written = 0;
    const auto Status = get(API, Partition, Buffer.base(), Size, Written);
    if (FAILED(Status))
      return whpError(diagnostic::WhpState, Status, getOperation());
    if (Written < x64::fp::XsaveBytes || Written > Size)
      return sizeError(Written, Size);
    const auto Packet = bytes().take_front(Written);
    if (auto E = decodeX64XsaveState(State, Packet)) {
      // Report only protocol metadata. The shared ISA codec remains the sole
      // authority for validation and never publishes a rejected packet.
      using namespace llvm::support::endian;
      const auto *P = Packet.data();
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          llvm::formatv(XsaveDecodeFailure, llvm::toString(std::move(E)),
                        getOperation(), Written,
                        read64le(P + x64::fp::XStateOffset),
                        read64le(P + x64::fp::XCompOffset),
                        read16le(P + x64::fp::ControlOffset),
                        read16le(P + x64::fp::OpcodeOffset),
                        read32le(P + x64::fp::MXCSROffset))
              .str());
    }
    return llvm::Error::success();
  }

private:
  llvm::Error sizeError(UINT32 Written, UINT32 Capacity) const {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        llvm::formatv(XsaveSizeFailure, diagnostic::FPState, getOperation(),
                      Written, Capacity, x64::fp::XsaveBytes,
                      x64::fp::MaxXsaveBytes)
            .str());
  }
  const char *getOperation() const {
    return Modern ? whp::operation::WHvGetVirtualProcessorState
                  : whp::operation::WHvGetVirtualProcessorXsaveState;
  }
  const char *setOperation() const {
    return Modern ? whp::operation::WHvSetVirtualProcessorState
                  : whp::operation::WHvSetVirtualProcessorXsaveState;
  }
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
