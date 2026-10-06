# Identification and image rebuilding are always available. Recovery observes
# a guest process, so it exists only where CPU execution is built.
#
# core/     orchestration and the format and protector registries
# format/   one directory per executable container
# packers/  one directory per protector
# dynamic/  guest process observation, per instruction set and guest system
set(_unpack_sources
  core/Identify.cpp
  core/Image.cpp
  core/Registry.cpp
  core/Unpack.cpp
  core/UnpackJSON.cpp
  core/UnpackNames.cpp
  format/pe/PEFormat.cpp
  format/pe/PEImage.cpp
  format/pe/PERebuild.cpp
  packers/upx/UPX.cpp
  packers/upx/UPXPE.cpp)
set(_unpack_components Support)
if(NEVERD_ENABLE_CPU_EMULATION OR NEVERD_ENABLE_DRIVER_EMULATION)
  list(APPEND _unpack_sources
    dynamic/Platform.cpp
    dynamic/ProcessTransfer.cpp)
  list(APPEND _unpack_components EmulationProcess)
endif()

add_neverd_component_library(NeverDUnpack
  ${_unpack_sources}
  LINK_COMPONENTS ${_unpack_components}
  LLVM_COMPONENTS Object Support)

if(NEVERD_ENABLE_CPU_EMULATION OR NEVERD_ENABLE_DRIVER_EMULATION)
  target_compile_definitions(NeverDUnpack PRIVATE NEVERD_UNPACK_EXECUTION=1)
endif()
if(WIN32)
  target_compile_definitions(NeverDUnpack PRIVATE NOMINMAX)
endif()
