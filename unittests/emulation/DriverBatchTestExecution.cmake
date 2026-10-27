# These original semantic matrices contain several independently bounded
# guests. Keep their guest limits; give the whole WHP batch its own allowance.
foreach(_neverd_driver_test IN LISTS NeverDNativeDriverTests_TESTS)
  string(FIND "${_neverd_driver_test}" "  # GetParam() = "
    _neverd_driver_annotation)
  if(_neverd_driver_annotation EQUAL -1)
    set(_neverd_driver_identity "${_neverd_driver_test}")
  else()
    string(SUBSTRING "${_neverd_driver_test}" 0
      ${_neverd_driver_annotation} _neverd_driver_identity)
  endif()
  foreach(_neverd_driver_variant
      whpLegacyPreferred whpLegacyRebased whpCheckedPreferred whpCheckedRebased)
    if(_neverd_driver_identity STREQUAL
        "Backends/KernelFileObjectExecution.OriginalBufferedNativeFileTransfers/${_neverd_driver_variant}")
      # Eighteen guests, each with the unchanged five-second driver bound.
      set_tests_properties("${_neverd_driver_test}" PROPERTIES TIMEOUT 120)
    elseif(_neverd_driver_identity STREQUAL
        "Backends/KernelIntegerFormattingExecution.OriginalImportsAndLookupPreserveAllValueBits/${_neverd_driver_variant}")
      # Four guests, each with the fixture's unchanged thirty-second bound.
      set_tests_properties("${_neverd_driver_test}" PROPERTIES TIMEOUT 150)
    endif()
  endforeach()
endforeach()
unset(_neverd_driver_test)
unset(_neverd_driver_annotation)
unset(_neverd_driver_identity)
unset(_neverd_driver_variant)
