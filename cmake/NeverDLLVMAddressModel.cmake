# Compile the scoped MC correction from a build-directory copy. Keep the
# recorded LLVM checkout and shared prebuilt packages unchanged.
function(neverd_scope_llvm_address_model)
    find_package(Git REQUIRED)
    get_target_property(_mc_source_dir LLVMMC SOURCE_DIR)
    set(_backend "${_mc_source_dir}/AddressModelBackend.cpp")
    set(_patch "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/llvm-aarch64-specifier-scope.patch")
    set(_overlay_root "${CMAKE_BINARY_DIR}/neverd-llvm-address-model")
    set(_overlay "${_overlay_root}/llvm/lib/MC/AddressModelBackend.cpp")
    set(_stamp "${_overlay_root}/inputs.sha256")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_backend}" "${_patch}")
    file(SHA256 "${_backend}" _backend_sha)
    file(SHA256 "${_patch}" _patch_sha)
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" _helper_sha)
    set(_inputs "${_backend_sha}\n${_patch_sha}\n${_helper_sha}\n")
    set(_previous "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
    endif()

    if(NOT EXISTS "${_overlay}" OR NOT _previous STREQUAL _inputs)
        file(MAKE_DIRECTORY "${_overlay_root}/llvm/lib/MC")
        configure_file("${_backend}" "${_overlay}" COPYONLY)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                    "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                    "${GIT_EXECUTABLE}" apply --no-index --check "${_patch}"
            WORKING_DIRECTORY "${_overlay_root}"
            RESULT_VARIABLE _check ERROR_VARIABLE _detail)
        if(_check EQUAL 0)
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env
                        "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                        "${GIT_EXECUTABLE}" apply --no-index "${_patch}"
                WORKING_DIRECTORY "${_overlay_root}"
                RESULT_VARIABLE _apply ERROR_VARIABLE _detail)
            if(NOT _apply EQUAL 0)
                message(FATAL_ERROR "Cannot scope LLVM MC specifiers: ${_detail}")
            endif()
        else()
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env
                        "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                        "${GIT_EXECUTABLE}" apply --no-index --reverse --check "${_patch}"
                WORKING_DIRECTORY "${_overlay_root}"
                RESULT_VARIABLE _fixed ERROR_VARIABLE _fixed_detail)
            if(NOT _fixed EQUAL 0)
                message(FATAL_ERROR
                    "LLVM AddressModelBackend does not match the scoped correction "
                    "or an already corrected source. ${_detail}${_fixed_detail}")
            endif()
        endif()
        file(WRITE "${_stamp}" "${_inputs}")
    endif()

    # Other MC sources remain in their original directory. Resolve their
    # relative paths before replacing just this one source with the copy.
    get_target_property(_sources LLVMMC SOURCES)
    set(_updated)
    set(_replaced 0)
    foreach(_source IN LISTS _sources)
        if(_source MATCHES "^\\$<")
            list(APPEND _updated "${_source}")
            continue()
        endif()
        get_filename_component(_absolute "${_source}" ABSOLUTE
            BASE_DIR "${_mc_source_dir}")
        if(_absolute STREQUAL _backend)
            list(APPEND _updated "${_overlay}")
            math(EXPR _replaced "${_replaced} + 1")
        else()
            list(APPEND _updated "${_absolute}")
        endif()
    endforeach()
    if(NOT _replaced EQUAL 1)
        message(FATAL_ERROR "Expected one LLVM MC address-model source")
    endif()
    set_property(TARGET LLVMMC PROPERTY SOURCES "${_updated}")
    set_source_files_properties("${_overlay}" TARGET_DIRECTORY LLVMMC
        PROPERTIES INCLUDE_DIRECTORIES "${_mc_source_dir}")
    message(STATUS "LLVM MC: compiling the scoped specifier correction from the build directory")
endfunction()
