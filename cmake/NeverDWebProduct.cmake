# A selected, relocatable Windows x64 CLI/C ABI/worker/MCP product.
option(NEVERD_BUILD_WEB_PRODUCT "Install the qualified Windows Web product" OFF)
if(NOT NEVERD_BUILD_WEB_PRODUCT)
  return()
endif()
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT NEVERD_ENABLE_WEB_ANALYSIS OR NOT NEVERD_ENABLE_WEB_JAVASCRIPT OR
   NOT NEVERD_ENABLE_WEB_ASAR OR NOT TARGET ICU::uc OR
   NOT TARGET neverd OR NOT TARGET neverd_shared OR
   NOT TARGET neverd-worker OR NOT TARGET neverd-web-mcp)
  message(FATAL_ERROR "Web product requires Windows x64, parser/ICU and all four product targets")
endif()
if(CMAKE_VERSION VERSION_LESS 3.21)
  message(FATAL_ERROR "Web runtime dependency packaging requires CMake 3.21 or newer")
endif()
set(NEVERD_MSVC_REDIST_DIRECTORY "" CACHE PATH "Matching licensed MSVC x64 CRT directory")
if(NOT EXISTS "${NEVERD_MSVC_REDIST_DIRECTORY}/msvcp140.dll" OR
   NOT EXISTS "${NEVERD_MSVC_REDIST_DIRECTORY}/vcruntime140.dll")
  message(FATAL_ERROR "Set NEVERD_MSVC_REDIST_DIRECTORY to the matching x64 redistributable CRT")
endif()
install(TARGETS neverd neverd_shared neverd-worker neverd-web-mcp
  RUNTIME_DEPENDENCY_SET neverd_web_product_runtime
  RUNTIME DESTINATION bin COMPONENT WebProduct
  LIBRARY DESTINATION bin COMPONENT WebProduct
  ARCHIVE DESTINATION lib COMPONENT WebProduct)
install(RUNTIME_DEPENDENCY_SET neverd_web_product_runtime
  DIRECTORIES "${ICU_ROOT}/bin64" "${NEVERD_MSVC_REDIST_DIRECTORY}"
  PRE_EXCLUDE_REGEXES "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*"
  POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
  RUNTIME DESTINATION bin COMPONENT WebProduct)
# ICU data is loaded by ICU itself and need not appear in the engine's import
# table. Always stage the exact data DLL alongside its exact 77.1 consumer.
install(FILES "${ICU_ROOT}/bin64/icuuc77.dll" "${ICU_ROOT}/bin64/icudt77.dll"
  DESTINATION bin COMPONENT WebProduct)
install(DIRECTORY "${NEVERD_MSVC_REDIST_DIRECTORY}/"
  DESTINATION bin COMPONENT WebProduct FILES_MATCHING PATTERN "*.dll")
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/sdk/"
  DESTINATION sdk COMPONENT WebProduct)
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/licenses/"
  DESTINATION share/neverd/licenses COMPONENT WebProduct)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/"
  DESTINATION share/neverd/licenses COMPONENT WebProduct)
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/NOTICE"
  "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  "${CMAKE_SOURCE_DIR}/docs/web-windows-product.md"
  "${CMAKE_SOURCE_DIR}/cmake/web-product-dependencies.json"
  DESTINATION . COMPONENT WebProduct)
install(FILES "${ICU_ROOT}/LICENSE"
  DESTINATION share/neverd/licenses/icu RENAME ICU-77.1-LICENSE COMPONENT WebProduct)
