add_neverd_tool(neverd-bench
  neverd-bench.cpp

  LINK_LIBS
  neverd_shared
)

option(NEVERD_BUILD_SOLVER_BENCH "Build the solver comparison and query export tool" OFF)
if(NEVERD_BUILD_SOLVER_BENCH)
  add_neverd_tool(neverd-solver-bench
    solver-bench.cpp
    LINK_COMPONENTS Solver Symbolic
  )
  if(NEVERD_ENABLE_Z3)
    add_custom_command(TARGET neverd-solver-bench POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:neverd-solver-bench>/licenses/z3"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSES/Z3.txt"
        "$<TARGET_FILE_DIR:neverd-solver-bench>/licenses/z3/LICENSE.txt"
      VERBATIM)
  endif()
endif()
