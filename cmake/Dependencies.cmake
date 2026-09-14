# Third-party code that is fetched rather than vendored. Pinned to a tag, never
# a branch, so a configure today and a configure next year build the same bytes.
include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
# Subsystems the host does not use stay out of the binary.
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.16
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# pop_link_sdl(<target>): link SDL3 statically and give the target its headers.
function(pop_link_sdl target)
  target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
endfunction()

# Video starts on macOS; mobile and other desktop hosts need separate
# cross-compilation and packaging support before enabling it.
set(RECOMP_VIDEO_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(RECOMP_VIDEO_DEFAULT ON)
endif()
option(RECOMP_VIDEO "Build the shared FFmpeg Bink and Smacker dependency" ${RECOMP_VIDEO_DEFAULT})

if(RECOMP_VIDEO)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "RECOMP_VIDEO currently supports macOS only")
  endif()
  include(ExternalProject)
  find_program(RECOMP_FFMPEG_MAKE NAMES make REQUIRED)
  set(RECOMP_FFMPEG_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg")
  set(RECOMP_FFMPEG_LIBRARIES
    "${RECOMP_FFMPEG_PREFIX}/lib/libavformat.61.dylib"
    "${RECOMP_FFMPEG_PREFIX}/lib/libavcodec.61.dylib"
    "${RECOMP_FFMPEG_PREFIX}/lib/libavutil.59.dylib")
  set(RECOMP_FFMPEG_CONFIGURE
    --prefix=${RECOMP_FFMPEG_PREFIX}
    --enable-shared --disable-static --disable-programs --disable-doc
    --disable-everything --disable-avdevice --disable-avfilter
    --disable-swscale --disable-swresample --disable-postproc --disable-network
    --enable-pic --install-name-dir=@rpath
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct,smacker,smackaud
    --enable-demuxer=bink,smacker --enable-protocol=file
    --disable-autodetect --disable-xlib --disable-libxcb --disable-sdl2
    --disable-iconv --disable-zlib --disable-bzlib --disable-lzma
    --disable-securetransport --disable-audiotoolbox --disable-videotoolbox
    --cc=${CMAKE_C_COMPILER})
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$" OR "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
    list(APPEND RECOMP_FFMPEG_CONFIGURE --disable-x86asm)
  endif()
  # FFmpeg uses a shell configure script and GNU make, not CMake or Ninja.
  # CMAKE_COMMAND is the same (venv) CMake that configured the kit.
  ExternalProject_Add(ffmpeg
    URL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
    URL_HASH SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND /bin/sh <SOURCE_DIR>/configure ${RECOMP_FFMPEG_CONFIGURE}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${RECOMP_FFMPEG_MAKE} -j8
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${RECOMP_FFMPEG_MAKE} install
    BUILD_BYPRODUCTS ${RECOMP_FFMPEG_LIBRARIES})
  # Imported include paths must exist at generation time, before installation.
  file(MAKE_DIRECTORY "${RECOMP_FFMPEG_PREFIX}/include")
  foreach(component avformat avcodec avutil)
    if(component STREQUAL "avutil")
      set(major 59)
    else()
      set(major 61)
    endif()
    add_library(ffmpeg::${component} SHARED IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${component} PROPERTIES
      IMPORTED_LOCATION "${RECOMP_FFMPEG_PREFIX}/lib/lib${component}.${major}.dylib"
      IMPORTED_SONAME "@rpath/lib${component}.${major}.dylib"
      INTERFACE_INCLUDE_DIRECTORIES "${RECOMP_FFMPEG_PREFIX}/include")
    add_dependencies(ffmpeg::${component} ffmpeg)
  endforeach()
endif()

# Object-library consumers must inherit both the headers and the dynamic link.
function(pop_link_video target)
  if(RECOMP_VIDEO)
    target_link_libraries(${target} PUBLIC ffmpeg::avformat ffmpeg::avcodec ffmpeg::avutil)
    target_compile_definitions(${target} PUBLIC RECOMP_HAVE_FFMPEG=1)
  endif()
endfunction()
