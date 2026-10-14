# One complete qualification inventory; disappearing tests fail CTest discovery.
set(_neverd_web_producer_tests
  WebBun.FullCompilerContainersMatchRecordedGoldenHashesWhenSupplied
  WebBun.CrossPlatformFullCompilerContainersWhenSupplied
  WebBunSourceMapCorpus.OptionalSixPlatformImagesRecoverIndependentOriginalSources
  WebNative.PinnedBunCompilerContainerEntersTheNativeLoaderWithoutExecution
  WebSEA.OptionalOfficialNodeSnapshotRemainsOpaque
  WebSEA.OptionalSixInjectedOfficialNodeImagesPreserveTheCompilerBlob
  WebNexe.OptionalPinnedProducerImagesPreserveIndependentInputs
  WebPkg.OptionalSixPlatformProducerImagesMatchIndependentInputs
  WebPkg.OptionalProducerPreprocessingPreservesStoredAndOriginalEvidence
  WebNexeSDK.OptionalProducerSourcesBootstrapAndManifestUseSharedConsumers
  WebPkgSDK.OptionalProducerSourcesPreludeAndManifestUseSharedConsumers)
set(_neverd_web_discovered_tests ${NeverDWebArtifactTests_TESTS}
  ${NeverDWebSourceMapTests_TESTS} ${NeverDWebNativeTests_TESTS}
  ${NeverDWebSDKTests_TESTS})
foreach(_test IN LISTS _neverd_web_producer_tests)
  list(FIND _neverd_web_discovered_tests "${_test}" _test_index)
  if(_test_index EQUAL -1)
    if("$ENV{NEVERD_REQUIRE_WEB_PRODUCER_CORPUS}" STREQUAL "1")
      message(FATAL_ERROR "Required Web producer qualification test missing: ${_test}")
    endif()
    continue()
  endif()
  if(_test MATCHES "^WebBunSourceMapCorpus\\.")
    set(_owner NeverDWebSourceMapTests)
  elseif(_test MATCHES "^WebNative\\.")
    set(_owner NeverDWebNativeTests)
  elseif(_test MATCHES "^Web(Nexe|Pkg)SDK\\.")
    set(_owner NeverDWebSDKTests)
  else()
    set(_owner NeverDWebArtifactTests)
  endif()
  set_tests_properties("${_test}" PROPERTIES
    LABELS "${_owner};NeverDWebProducerCorpus")
endforeach()
unset(_neverd_web_producer_tests)
unset(_neverd_web_discovered_tests)
unset(_test_index)
unset(_owner)
unset(_test)
