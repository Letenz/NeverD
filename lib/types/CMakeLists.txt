# The type libraries of system headers (HeaderLibraries.def), embedded so
# that reading one reads no file at run time.
set(NEVERD_HEADER_LIBRARIES WindowsKernelX64)
set(NEVERD_HEADER_LIBRARY_INCS)
foreach(Library IN LISTS NEVERD_HEADER_LIBRARIES)
  set(Source "${CMAKE_CURRENT_SOURCE_DIR}/headers/${Library}.h")
  set(Embedded "${CMAKE_CURRENT_BINARY_DIR}/headers/${Library}.inc")
  add_custom_command(
    OUTPUT "${Embedded}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${Source}" "-DOUTPUT=${Embedded}"
            -P "${PROJECT_SOURCE_DIR}/cmake/NeverDEmbedBytes.cmake"
    DEPENDS "${Source}" "${PROJECT_SOURCE_DIR}/cmake/NeverDEmbedBytes.cmake"
    COMMENT "Embedding the ${Library} type library"
    VERBATIM)
  list(APPEND NEVERD_HEADER_LIBRARY_INCS "${Embedded}")
endforeach()

add_neverd_component_library(NeverDTypes
  CTypeParser.cpp
  CTypeRender.cpp
  GoTypeParser.cpp
  GoTypeRender.cpp
  HeaderLibrary.cpp
  JavaScriptTypeRender.cpp
  LocalTypes.cpp
  LuaTypeRender.cpp
  PDBTypeImport.cpp
  PythonTypeRender.cpp
  RustTypeParser.cpp
  RustTypeRender.cpp
  TypeFit.cpp
  TypeJSON.cpp
  TypeLayout.cpp
  TypeRender.cpp
  ViewPlan.cpp
  ${NEVERD_HEADER_LIBRARY_INCS}
)

target_include_directories(NeverDTypes PRIVATE
                           "${CMAKE_CURRENT_BINARY_DIR}/headers")
