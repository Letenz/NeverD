add_neverd_tool(neverd-bytecode
  neverd-bytecode.cpp
  LINK_LIBS neverd_shared
)
if(NOT APPLE)
  target_link_libraries(neverd-bytecode PRIVATE NeverDSupport)
endif()

add_neverd_tool(neverd-bytecode-observe
  BytecodeObserve.cpp
  LINK_LIBS NeverDBytecodeAnalysis NeverDSymbolic NeverDSolver
)
if(NEVERD_ENABLE_CPU_EMULATION)
  add_neverd_tool(neverd-process-probe
    ProcessProbe.cpp
    LINK_LIBS NeverDEmulationProcess
  )
  neverd_sign_hypervisor(neverd-process-probe)
endif()
