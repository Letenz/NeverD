# Compile the scoped MC correction from a build-directory copy. Keep the
# recorded LLVM checkout and shared prebuilt packages unchanged.
function(neverd_scope_llvm_address_model)
    find_package(Git REQUIRED)
    get_target_property(_mc_source_dir LLVMMC SOURCE_DIR)
    set(_backend "${_mc_source_dir}/AddressModelBackend.cpp")
    set(_patch_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches")
    set(_scope_patches
        "${_patch_root}/llvm-aarch64-specifier-scope.patch"
        "${_patch_root}/llvm-aarch64-upstream-specifier-scope.patch")
    set(_i386_patches "${_patch_root}/llvm-i386-got-base.patch")
    set(_macho_patches "${_patch_root}/llvm-macho-gotpcrel-data.patch")
    set(_overlay_root "${CMAKE_BINARY_DIR}/neverd-llvm-address-model")
    set(_overlay "${_overlay_root}/llvm/lib/MC/AddressModelBackend.cpp")
    set(_stamp "${_overlay_root}/inputs.sha256")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_backend}" ${_scope_patches} ${_i386_patches} ${_macho_patches})
    file(SHA256 "${_backend}" _backend_sha)
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" _helper_sha)
    set(_inputs "${_backend_sha}\n${_helper_sha}\n")
    foreach(_patch IN LISTS _scope_patches _i386_patches _macho_patches)
        file(SHA256 "${_patch}" _patch_sha)
        string(APPEND _inputs "${_patch_sha}\n")
    endforeach()
    set(_previous "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
    endif()

    if(NOT EXISTS "${_overlay}" OR NOT _previous STREQUAL _inputs)
        file(MAKE_DIRECTORY "${_overlay_root}/llvm/lib/MC")
        configure_file("${_backend}" "${_overlay}" COPYONLY)
        # A ceiling stops repository discovery, but an inherited GIT_DIR or
        # GIT_WORK_TREE bypasses discovery. Git then silently skips paths
        # outside that worktree even with --no-index, reporting success.
        set(_isolated_git "${CMAKE_COMMAND}" -E env
            --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_COMMON_DIR
            --unset=GIT_INDEX_FILE --unset=GIT_PREFIX
            "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
            "${GIT_EXECUTABLE}")
        # The fork can already contain only some corrections. Check each
        # semantic correction independently; the two scope patches recognize
        # the original overlay spelling and the fork's equivalent guard.
        foreach(_group IN ITEMS _scope_patches _i386_patches _macho_patches)
            set(_matched FALSE)
            set(_details "")
            foreach(_patch IN LISTS ${_group})
                execute_process(
                    COMMAND ${_isolated_git} apply --no-index --check "${_patch}"
                    WORKING_DIRECTORY "${_overlay_root}"
                    RESULT_VARIABLE _check ERROR_VARIABLE _detail)
                if(_check EQUAL 0)
                    file(SHA256 "${_overlay}" _before_sha)
                    execute_process(
                        COMMAND ${_isolated_git} apply --no-index "${_patch}"
                        WORKING_DIRECTORY "${_overlay_root}"
                        RESULT_VARIABLE _apply ERROR_VARIABLE _detail)
                    if(NOT _apply EQUAL 0)
                        message(FATAL_ERROR "Cannot scope LLVM MC correction: ${_detail}")
                    endif()
                    file(SHA256 "${_overlay}" _corrected_sha)
                    if(_corrected_sha STREQUAL _before_sha)
                        message(FATAL_ERROR "LLVM MC correction was skipped by Git")
                    endif()
                    set(_matched TRUE)
                    break()
                endif()
                execute_process(
                    COMMAND ${_isolated_git} apply --no-index --reverse --check "${_patch}"
                    WORKING_DIRECTORY "${_overlay_root}"
                    RESULT_VARIABLE _fixed ERROR_VARIABLE _fixed_detail)
                if(_fixed EQUAL 0)
                    set(_matched TRUE)
                    break()
                endif()
                string(APPEND _details "${_detail}${_fixed_detail}")
            endforeach()
            if(NOT _matched)
                message(FATAL_ERROR
                    "LLVM AddressModelBackend does not match the scoped correction "
                    "or an already corrected source (${_group}). ${_details}")
            endif()
        endforeach()
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
