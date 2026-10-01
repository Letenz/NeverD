add_neverd_unittest(NeverDBytecodeAnalysisTests
  BytecodeDecoderTests.cpp
  BytecodeCLITests.cpp
  LINK_COMPONENTS BytecodeAnalysis IRMed IRHigh BackendC BackendLLVM Support
)
target_compile_definitions(NeverDBytecodeAnalysisTests PRIVATE
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_BYTECODE_BINARY="$<TARGET_FILE:neverd-bytecode>"
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
)
add_dependencies(NeverDBytecodeAnalysisTests neverd-bytecode)

add_neverd_unittest(NeverDLowIRUndefinedIndependenceTests
  LowIRUndefinedIndependenceTests.cpp
  BinaryUndefinedIndependenceTests.cpp
  LINK_COMPONENTS InterpreterSpecialization Decode Lift
)

add_neverd_unittest(NeverDOriginalBinaryUndefinedIndependenceTests
  OriginalBinaryUndefinedIndependenceTests.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization
)

add_neverd_unittest(NeverDLowIRRefinementTests
  LowIRRefinementTests.cpp
  LowIRLoopAlignmentTests.cpp
  InterpreterMachineStateModelTests.cpp
  BinaryLowIRRefinementTests.cpp
  LINK_COMPONENTS BinaryInterpreterSpecialization
)

add_neverd_unittest(NeverDLLVMInterpreterModelTests
  LLVMInterpreterModelTests.cpp
  LLVMInterpreterModelCompileTests.cpp
  LLVMInterpreterModelGuardTests.cpp
  LLVMInterpreterModelInitializationTests.cpp
  LLVMInterpreterModelLoopTests.cpp
  LLVMInterpreterModelRejectionTests.cpp
  LLVMInterpreterModelShiftTests.cpp
  LINK_COMPONENTS LLVMInterpreterModel
  LINK_LIBS LLVMAsmParser LLVMIRReader
)
target_compile_definitions(NeverDLLVMInterpreterModelTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDInterpreterLLVMRefinementTests
  InterpreterLLVMRefinementTests.cpp
  InterpreterLLVMRefinementLoopTests.cpp
  InterpreterLLVMRefinementCompileTests.cpp
  LINK_COMPONENTS InterpreterLLVMRefinement
)
target_compile_definitions(NeverDInterpreterLLVMRefinementTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/unittests/lift"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)

add_neverd_unittest(NeverDInterpreterSpecializationTests
  InterpreterSpecializationTests.cpp
  NativeStackSpecializationTests.cpp
  NativeStackControlTests.cpp
  NativeUndefinedIndependenceTests.cpp
  MemoryCallSpecializationTests.cpp
  ControlStateRecoveryTests.cpp
  MaskedControlRelationTests.cpp
  CanonicalControlFieldsTests.cpp
  ControlDiscoveryTests.cpp
  FiniteValuesTests.cpp
  FiniteQueryCacheTests.cpp
  LINK_COMPONENTS InterpreterSpecialization
)

add_neverd_unittest(NeverDDevirtualizationSourceTests
  DevirtualizationSourceTests.cpp
  VMShapeSourceTests.cpp
  RecoverySourcePublicationTests.cpp
  PrivateSourceFrameTests.cpp
  NativeRegisterRecoveryTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDDevirtualizationSourceTests PRIVATE
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
add_dependencies(NeverDDevirtualizationSourceTests neverd)

add_neverd_unittest(NeverDInterpreterMachineStateTests
  InterpreterMachineStateTests.cpp
  MachineControlSourceTests.cpp
  MachineMemoryCallSourceTests.cpp
  LINK_COMPONENTS Pipeline BackendC Support
)
target_compile_definitions(NeverDInterpreterMachineStateTests PRIVATE
  TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  NEVERD_BINARY="$<TARGET_FILE:neverd>"
  NEVERD_TEST_CLANG="${NEVERD_TEST_CLANG_EXECUTABLE}"
)
add_dependencies(NeverDInterpreterMachineStateTests neverd)
