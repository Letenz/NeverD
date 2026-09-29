# CPU execution can be built independently of the Windows driver environment.
add_neverd_component_library(NeverDEmulationCore
  GuestMemory.cpp
  core/ExecutionBackend.cpp
  core/Registers.cpp
  core/CheckedBackend.cpp
  core/PhysicalMemory.cpp)

add_neverd_component_library(NeverDEmulationNative
  arch/x86_64/X64PageTables.cpp
  arch/aarch64/AArch64PageTables.cpp
  backends/kvm/KvmMachine.cpp
  backends/kvm/KvmAArch64Machine.cpp
  backends/whp/WhpMachine.cpp
  backends/whp/WhpAArch64Machine.cpp
  LINK_COMPONENTS EmulationCore)

if(NEVERD_EMULATION_BACKEND_UNICORN)
  add_neverd_component_library(NeverDEmulationUnicorn
    unicorn/UnicornBackend.cpp
    unicorn/UnicornMachine.cpp
    unicorn/UnicornArchitecture.cpp
    LINK_COMPONENTS EmulationCore
    LINK_LIBS unicorn)
else()
  add_neverd_component_library(NeverDEmulationUnicorn
    unicorn/UnicornUnavailable.cpp
    LINK_COMPONENTS EmulationCore)
endif()

add_neverd_component_library(NeverDEmulationCPU
  core/BackendRegistry.cpp
  arch/x86_64/CheckedX64Backend.cpp
  arch/aarch64/CheckedAArch64Backend.cpp
  LINK_COMPONENTS EmulationCore EmulationNative EmulationUnicorn)

if(NEVERD_EMULATION_BACKEND_UNICORN)
  target_compile_definitions(NeverDEmulationCPU PRIVATE NEVERD_EMULATION_UNICORN=1)
endif()

if(WIN32)
  foreach(_component Core Native Unicorn CPU)
    target_compile_definitions(NeverDEmulation${_component} PRIVATE NOMINMAX)
  endforeach()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NEVERD_EMULATION_BACKEND_KVM)
  target_compile_definitions(NeverDEmulationNative PRIVATE NEVERD_EMULATION_KVM=1)
endif()
if(WIN32 AND NEVERD_EMULATION_BACKEND_WHP)
  target_compile_definitions(NeverDEmulationNative PRIVATE NEVERD_EMULATION_WHP=1)
endif()
if(NOT NEVERD_ENABLE_DRIVER_EMULATION)
  return()
endif()

add_neverd_component_library(NeverDEmulation
  DriverResult.cpp
  DriverScenario.cpp
  DriverSession.cpp
  windows/WindowsX64ExecutionPolicy.cpp
  windows/DriverImage.cpp
  windows/KernelExportRegistry.cpp
  windows/KernelException.cpp
  windows/KernelSEH.cpp
  windows/KernelSEHGS.cpp
  windows/KernelSEHEpilogue.cpp
  windows/GuardControlFlow.cpp
  windows/KernelFramework.cpp
  windows/KernelFrameworkInterrupts.cpp
  windows/KernelFrameworkLocks.cpp
  windows/KernelModelFrameworkLocks.cpp
  windows/KernelModelFrameworkInterrupts.cpp
  windows/KernelFrameworkPower.cpp
  windows/KernelFrameworkPowerPolicy.cpp
  windows/KernelFrameworkPowerWake.cpp
  windows/KernelFrameworkPoFx.cpp
  windows/KernelModelPowerPolicy.cpp
  windows/KernelModelPowerEvents.cpp
  windows/KernelUsbIdle.cpp
  windows/KernelModelFrameworkUsbIdle.cpp
  windows/KernelModelUsbIdle.cpp
  windows/KernelModelProviderReceipt.cpp
  windows/KernelModelDriverIRP.cpp
  windows/KernelModelDriverIRPRequests.cpp
  windows/KernelPoFx.cpp
  windows/KernelModelPoFx.cpp
  windows/KernelModelPoFxPolicy.cpp
  windows/KernelFrameworkControl.cpp
  windows/KernelFrameworkFiles.cpp
  windows/KernelFrameworkQueue.cpp
  windows/KernelFrameworkRequests.cpp
  windows/KernelFrameworkRequestAccessors.cpp
  windows/KernelModelFramework.cpp
  windows/KernelModelFrameworkRequests.cpp
  windows/KernelModelExports.cpp
  windows/KernelModel.cpp
  windows/KernelModelIO.cpp
  windows/KernelModelUserMemory.cpp
  windows/KernelModelPhysicalMemory.cpp
  windows/KernelModelMDL.cpp
  windows/KernelModelMDLUserMappings.cpp
  windows/KernelModelIRPStack.cpp
  windows/KernelModelDeviceStack.cpp
  windows/KernelModelPnpDevices.cpp
  windows/KernelModelPnpRequests.cpp
  windows/KernelModelPnpCompletion.cpp
  windows/KernelModelPowerRequests.cpp
  windows/KernelModelPowerCompletion.cpp
  windows/KernelModelResources.cpp
  windows/KernelInterrupts.cpp
  windows/KernelModelInterrupts.cpp
  windows/KernelModelInterruptEvents.cpp
  windows/KernelResources.cpp
  windows/KernelPhysicalMemory.cpp
  windows/KernelDMA.cpp
  windows/KernelDMAChannels.cpp
  windows/KernelDMAEvents.cpp
  windows/KernelModelDMA.cpp
  windows/KernelModelDMAChannels.cpp
  windows/KernelModelDMATransfers.cpp
  windows/KernelMMIO.cpp
  windows/KernelModelRemoveLocks.cpp
  windows/KernelRemoveLocks.cpp
  windows/KernelModelRuntime.cpp
  windows/KernelModelScheduling.cpp
  windows/KernelModelFrameworkSynchronization.cpp
  windows/KernelModelSpinLocks.cpp
  windows/KernelModelIRQL.cpp
  windows/KernelModelCancellation.cpp
  windows/KernelScheduler.cpp
  windows/KernelDispatcher.cpp
  windows/KernelAPIIRQL.cpp
  windows/DeviceLifecycle.cpp
  windows/KernelRegistry.cpp
  LINK_COMPONENTS Loader EmulationCPU)
if(WIN32)
  target_compile_definitions(NeverDEmulation PRIVATE NOMINMAX)
endif()
