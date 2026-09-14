# The guest runtime. Object libraries, so every source is linked into every
# consumer exactly as the shell scripts linked every object.
set(POP_RUNTIME_SOURCES
  memory.cpp loader.cpp imports.cpp cpu.cpp profile.cpp kernel32.cpp kernel32_wide.cpp resources.cpp oleaut32.cpp
  mods_seam.cpp user32.cpp user32_wide.cpp user32_vcl.cpp misc.cpp misc_dlls.cpp comctl32.cpp gdi32.cpp layout.cpp)

add_library(recomp_runtime OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_runtime PUBLIC recomp_platform)
target_compile_options(recomp_runtime PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime 2)

# POPM_TESTING compiles the test-only entry points (loader reset, owner
# counter seam, scheduler latch reset). None exists in a real build.
if(NOT IOS AND NOT ANDROID)
add_library(recomp_runtime_testing OBJECT ${POP_RUNTIME_SOURCES})
target_include_directories(recomp_runtime_testing PUBLIC ${POP_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recomp_runtime_testing PUBLIC recomp_platform)
target_compile_definitions(recomp_runtime_testing PUBLIC POPM_TESTING=1)
target_compile_options(recomp_runtime_testing PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_runtime_testing 1)
endif()

# Linked by the hosts, the fixture and the mod tests, not by the unit tests.
add_library(recomp_snapshot OBJECT snapshot.cpp)
target_link_libraries(recomp_snapshot PUBLIC recomp_runtime)
target_compile_options(recomp_snapshot PRIVATE ${POP_WARN_HOST})
pop_optimize(recomp_snapshot 2)

# The suite spawns itself for the abort check; portable since sub-project 3.
if(NOT IOS AND NOT ANDROID)
add_executable(runtime_tests tests/runtime_tests.cpp tests/stub_recomp_call.cpp)
target_link_libraries(runtime_tests PRIVATE recomp_platform recomp_runtime)
target_compile_options(runtime_tests PRIVATE ${POP_WARN_HOST})
pop_optimize(runtime_tests 1)
pop_test_binary(runtime_tests)
add_test(NAME runtime_tests COMMAND runtime_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(runtime_tests PROPERTIES LABELS game ENVIRONMENT RECOMP_PYTHON=${Python3_EXECUTABLE})
endif()

if(POP_REAL_GEN AND NOT IOS AND NOT ANDROID)
  # The headless parity fixture: null host, real generated code, core mods installed.
  add_executable(pop_fixture fixture.cpp)
  target_link_libraries(pop_fixture PRIVATE recomp_platform recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua)
  target_compile_options(pop_fixture PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture 1)
  pop_link_gen(pop_fixture)
  if(TARGET core_mods)
    add_dependencies(pop_fixture core_mods)
  endif()

  # CALL_FN takes an address token (without 0x), not a numeric expression.
  # Select the first generated entry; define the suite only when one exists.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${POP_GEN_DIR}/funcs.h)
  file(STRINGS ${POP_GEN_DIR}/funcs.h PROFILE_FIRST_INDEX
    REGEX "^#define FIDX_[0-9a-f]+ 0u$" LIMIT_COUNT 1)
  if(PROFILE_FIRST_INDEX)
    add_executable(profile_tests tests/profile_tests.cpp)
    string(REGEX REPLACE "^#define FIDX_([0-9a-f]+) 0u$" "\\1"
      PROFILE_TARGET_TOKEN "${PROFILE_FIRST_INDEX}")
    target_compile_definitions(profile_tests PRIVATE RECOMP_PROFILE_TARGET=${PROFILE_TARGET_TOKEN})
    target_link_libraries(profile_tests PRIVATE recomp_platform recomp_runtime recomp_dx_null)
    target_compile_options(profile_tests PRIVATE ${POP_WARN_HOST})
    pop_optimize(profile_tests 1)
    pop_link_gen(profile_tests)
    pop_test_binary(profile_tests)
    add_test(NAME profile_tests_disabled COMMAND profile_tests disabled WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(profile_tests_disabled PROPERTIES LABELS game ENVIRONMENT RECOMP_PROFILE=0)
    add_test(NAME profile_tests_enabled COMMAND profile_tests enabled WORKING_DIRECTORY ${POP_ROOT})
    set_tests_properties(profile_tests_enabled PROPERTIES LABELS game ENVIRONMENT RECOMP_PROFILE=1)
  else()
    message(STATUS "profile_tests: the translation has no FIDX entry; suite not defined")
  endif()
endif()

if(POP_TRACE_DIR AND NOT IOS AND NOT ANDROID)
  # Caller-supplied trace wrappers over a snapshot of gen/: the override header
  # is force-included ahead of funcs.h so every FN_<addr> macro is already set.
  file(GLOB POP_TRACE_SOURCES CONFIGURE_DEPENDS ${POP_TRACE_DIR}/gen/chunk_*.c ${POP_TRACE_DIR}/gen/table.c)
  add_library(recomp_gen_trace OBJECT ${POP_TRACE_SOURCES} ${POP_TRACE_DIR}/trace_wrappers.c)
  target_include_directories(recomp_gen_trace PRIVATE ${POP_TRACE_DIR} ${POP_TRACE_DIR}/gen ${POP_ROOT} ${POP_ROOT}/runtime)
  target_compile_options(recomp_gen_trace PRIVATE ${POP_WARN_GEN} -include ${POP_TRACE_DIR}/trace_override.h)
  pop_optimize(recomp_gen_trace 1)
  add_executable(pop_fixture_trace fixture.cpp)
  target_link_libraries(pop_fixture_trace PRIVATE recomp_platform recomp_runtime recomp_snapshot recomp_dx_null recomp_mods lua recomp_gen_trace)
  target_include_directories(pop_fixture_trace PRIVATE ${POP_TRACE_DIR}/gen)
  target_compile_options(pop_fixture_trace PRIVATE ${POP_WARN_HOST})
  pop_optimize(pop_fixture_trace 1)
endif()
