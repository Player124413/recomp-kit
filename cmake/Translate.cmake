# The translated game lives only in the developer's build/recomp/gen; the
# kit never tracks generated code (spec section 11).
set(POP_GEN_DIR ${POP_OUT}/gen)
if(POP_TRANSLATE STREQUAL "STUB")
  # A link-only translation for builds without game code (CI).
  set(POP_GEN_DIR ${CMAKE_BINARY_DIR}/stub-gen)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gen_stub_translation.py --out ${POP_GEN_DIR}
    RESULT_VARIABLE POP_STUB_RESULT)
  if(NOT POP_STUB_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/gen_stub_translation.py failed")
  endif()
endif()
set(POP_HAVE_GEN OFF)
# POP_REAL_GEN: the translation is the game's, not the link-only stub. Tests
# that call into generated functions by address are defined only then.
set(POP_REAL_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
  if(NOT POP_TRANSLATE STREQUAL "STUB")
    set(POP_REAL_GEN ON)
  endif()
  message(STATUS "Translation: ${POP_GEN_DIR}")
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but no translation: run tools/build.py --regenerate")
else()
  message(STATUS "No translation found: hosts and game-backed tests are not defined")
endif()

if(POP_HAVE_GEN)
  file(GLOB POP_GEN_SOURCES CONFIGURE_DEPENDS ${POP_GEN_DIR}/chunk_*.c ${POP_GEN_DIR}/table.c)
  add_library(recomp_gen STATIC ${POP_GEN_SOURCES})
  set_target_properties(recomp_gen PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${POP_OUT} OUTPUT_NAME recomp_gen)
  # -I<gen> for x86.h beside the sources, -I<root> for the canonical copy,
  # -I<runtime> for intrinsics.h: the same three the shell script passed.
  target_include_directories(recomp_gen PRIVATE ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/runtime)
  target_include_directories(recomp_gen INTERFACE ${POP_GEN_DIR})
  target_compile_options(recomp_gen PRIVATE ${POP_WARN_GEN})
  pop_optimize(recomp_gen 2)
endif()

# The portable spelling of -Wl,-force_load: every generated object is kept
# whether or not anything references it, because the dispatch table is
# reached by address.
function(pop_link_gen target)
  target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>")
  target_include_directories(${target} PRIVATE ${POP_GEN_DIR})
endfunction()
