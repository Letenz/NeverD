cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(NeverDDesktop VERSION 3389.0.1 LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  include(CTest)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../neverd-worker" "${CMAKE_BINARY_DIR}/worker")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools Svg
  Sql Concurrent)
# Qt 6.8 exports private targets from the public module packages. Newer Qt
# releases ship separate private packages; KDDockWidgets needs the matching
# private headers in either layout.
foreach(_neverd_private_module Gui Widgets)
  if(NOT TARGET Qt6::${_neverd_private_module}Private)
    find_package(Qt6 6.8 REQUIRED COMPONENTS ${_neverd_private_module}Private)
  endif()
endforeach()
qt_standard_project_setup(REQUIRES 6.8)
include(FetchContent)
set(KDDockWidgets_FRONTENDS "qtwidgets" CACHE STRING "" FORCE)
set(KDDockWidgets_QT6 ON CACHE BOOL "" FORCE)
set(KDDockWidgets_STATIC ON CACHE BOOL "" FORCE)
set(KDDockWidgets_QML_MODULE OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_TESTS OFF CACHE BOOL "" FORCE)
set(KDDockWidgets_NO_SPDLOG ON CACHE BOOL "" FORCE)
set(KDDockWidgets_LINTER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(neverd_docking
  URL https://github.com/KDAB/KDDockWidgets/archive/refs/tags/v2.4.1.tar.gz
  URL_HASH SHA256=5cd9495d9316cf6cc937bb328a7fd491c3248ef2469cfa42511f1039f6148aca
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(neverd_docking)

# Worker transport, request dispatch and MCP bridges: no widgets.
add_library(neverd_gui_core STATIC EngineClient.cpp QueryService.cpp
  LibraryCodeView.cpp mcp/McpConnectionManager.cpp mcp/GuiSessionBroker.cpp)
target_include_directories(neverd_gui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(neverd_gui_core PUBLIC cxx_std_20)
target_link_libraries(neverd_gui_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network)

# The workbench: views, windows and the command set.
file(GLOB NEVERD_GUI_APP_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp")
add_library(neverd_gui_app STATIC ${NEVERD_GUI_APP_SOURCES})
target_include_directories(neverd_gui_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/app"
  # Listing roles and address classes are shared with the worker.
  "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-worker"
  "${neverd_docking_SOURCE_DIR}/src")
target_link_libraries(neverd_gui_app PUBLIC neverd_gui_core Qt6::Widgets Qt6::Svg
  Qt6::Sql Qt6::Concurrent KDAB::kddockwidgets)
file(GLOB NEVERD_GUI_ICONS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.svg")
qt_add_resources(neverd_gui_app "workbench_assets" PREFIX /neverd
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
  FILES ${NEVERD_GUI_ICONS} "${CMAKE_CURRENT_SOURCE_DIR}/resources/theme.qss")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-dark.svg"
  PROPERTIES QT_RESOURCE_ALIAS "icons/app.svg")
qt_add_resources(neverd_gui_app "brand_assets" PREFIX /neverd
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/assets/neverd-logo-dark.svg")

qt_add_executable(neverd-gui WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(neverd-gui PRIVATE neverd_gui_app)

file(GLOB NEVERD_TRANSLATIONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.ts")
if(NEVERD_TRANSLATIONS)
  qt_add_translations(neverd-gui TS_FILES ${NEVERD_TRANSLATIONS}
    SOURCE_TARGETS neverd-gui neverd_gui_app neverd_gui_core
    QM_FILES_OUTPUT_VARIABLE NEVERD_QM_FILES)
  qt_add_resources(neverd-gui "app_translations" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_QM_FILES})
endif()
add_dependencies(neverd-gui neverd-worker)
install(TARGETS neverd-gui RUNTIME DESTINATION bin BUNDLE DESTINATION .)
qt_generate_deploy_app_script(TARGET neverd-gui OUTPUT_SCRIPT deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/neverd-mcp" DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/server.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/transport.py" DESTINATION bin)

if(BUILD_TESTING)
  option(NEVERD_GUI_CI_DIAGNOSTICS
    "Write file diagnostics for the Windows GUI controller test runner" OFF)
  set(_neverd_controller_test_log_args)
  if(NEVERD_GUI_CI_DIAGNOSTICS)
    if(NOT WIN32 OR NOT "$ENV{CI}" STREQUAL "true" OR
       NOT "$ENV{GITHUB_ACTIONS}" STREQUAL "true")
      message(FATAL_ERROR "GUI contract diagnostics require Windows GitHub Actions")
    endif()
    set(_neverd_gui_ci_log_dir "${CMAKE_CURRENT_BINARY_DIR}/ci-diagnostics")
    file(MAKE_DIRECTORY "${_neverd_gui_ci_log_dir}")
    set(_neverd_controller_test_log_args
      -o "${_neverd_gui_ci_log_dir}/controller.txt,txt"
      -o "${_neverd_gui_ci_log_dir}/controller.xml,xml"
      -o "-,txt")
  endif()
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  target_sources(neverd-gui PRIVATE tests/WidgetsProbe.cpp)
  target_compile_definitions(neverd-gui PRIVATE NEVERD_GUI_TEST_PROBES=1)
  target_link_libraries(neverd-gui PRIVATE Qt6::Test)

  qt_add_executable(neverd-gui-query-tests tests/QueryServiceTests.cpp QueryService.cpp)
  target_link_libraries(neverd-gui-query-tests PRIVATE Qt6::Core Qt6::Test)
  target_include_directories(neverd-gui-query-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME NeverDGuiQueries COMMAND neverd-gui-query-tests)
  set_tests_properties(NeverDGuiQueries PROPERTIES TIMEOUT 60)

  qt_add_executable(neverd-gui-library-view-tests tests/LibraryCodeViewTests.cpp)
  target_link_libraries(neverd-gui-library-view-tests PRIVATE neverd_gui_core Qt6::Test)
  add_test(NAME NeverDGuiLibraryView COMMAND neverd-gui-library-view-tests)
  set_tests_properties(NeverDGuiLibraryView PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

  qt_add_executable(neverd-gui-unit-tests tests/WorkbenchUnitTests.cpp)
  target_link_libraries(neverd-gui-unit-tests PRIVATE neverd_gui_app Qt6::Test)
  add_test(NAME NeverDGuiUnits COMMAND neverd-gui-unit-tests)
  set_tests_properties(NeverDGuiUnits PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

  if(TARGET neverd-worker-test-server)
    qt_add_executable(neverd-gui-tests tests/WorkbenchTests.cpp)
    target_link_libraries(neverd-gui-tests PRIVATE neverd_gui_app Qt6::Test)
    target_compile_definitions(neverd-gui-tests PRIVATE
      TEST_WORKER="$<TARGET_FILE:neverd-worker-test-server>")
    add_dependencies(neverd-gui-tests neverd-worker-test-server)
    add_test(NAME NeverDGuiController COMMAND neverd-gui-tests
      ${_neverd_controller_test_log_args})
    set_tests_properties(NeverDGuiController PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    add_test(NAME NeverDGuiWidgets COMMAND neverd-gui --widgets-test
      --worker $<TARGET_FILE:neverd-worker-test-server>)
    set_tests_properties(NeverDGuiWidgets PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
  endif()

  qt_add_executable(neverd-mcp-qt-tests mcp/tests/test_mcp.cpp)
  target_link_libraries(neverd-mcp-qt-tests PRIVATE neverd_gui_core Qt6::Test)
  target_include_directories(neverd-mcp-qt-tests PRIVATE mcp)
  target_compile_definitions(neverd-mcp-qt-tests PRIVATE
    TEST_PYTHON="${Python3_EXECUTABLE}" TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/mcp/tests/fake_mcp.py")
  add_test(NAME NeverDMcpQt COMMAND neverd-mcp-qt-tests)
  set_tests_properties(NeverDMcpQt PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1" TIMEOUT 30)
  add_test(NAME NeverDMcpProtocol COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/../neverd-mcp/tests" -v)
  if(NEVERD_WORKER_REAL_ENGINE_TESTS)
    add_test(NAME NeverDGuiAnalysis COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_probe_test.py"
      "$<TARGET_FILE:neverd-gui>" "$<TARGET_FILE:neverd_shared>")
    set_tests_properties(NeverDGuiAnalysis PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()
  add_test(NAME NeverDGuiSmoke COMMAND neverd-gui --smoke-test)
  set_tests_properties(NeverDGuiSmoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
  if(NEVERD_TRANSLATIONS)
    qt_add_executable(neverd-gui-language-tests tests/LanguageSwitchTests.cpp)
    target_link_libraries(neverd-gui-language-tests PRIVATE neverd_gui_app Qt6::Test)
    qt_add_resources(neverd-gui-language-tests "test_translations" PREFIX /i18n
      BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${NEVERD_QM_FILES})
    add_test(NAME NeverDGuiLanguages COMMAND neverd-gui-language-tests)
    set_tests_properties(NeverDGuiLanguages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
  endif()
  # These are console test runners, even though they exercise Qt GUI objects.
  # Keep their diagnostics connected to CTest's stdout on Windows.
  foreach(_neverd_gui_test IN ITEMS neverd-gui-tests neverd-gui-unit-tests
      neverd-gui-library-view-tests neverd-gui-query-tests neverd-mcp-qt-tests
      neverd-gui-language-tests)
    if(TARGET ${_neverd_gui_test})
      set_target_properties(${_neverd_gui_test} PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
  endforeach()
endif()
