add_neverd_tool(neverd-bytecode
  neverd-bytecode.cpp
  LINK_LIBS neverd_shared
)
if(NOT APPLE)
  target_link_libraries(neverd-bytecode PRIVATE NeverDSupport)
endif()
