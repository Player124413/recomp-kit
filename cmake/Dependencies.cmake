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

# Video is enabled on the hosts with shared-library packaging support.
set(RECOMP_VIDEO_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR IOS OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(RECOMP_VIDEO_DEFAULT ON)
elseif(WIN32)
  # FFmpeg's configure needs the MSYS2 shell and GNU make on PATH. The
  # native MinGW compiler path is supported; clang-cl/MSVC is out of scope.
  find_program(RECOMP_FFMPEG_SHELL NAMES bash PATHS ENV PATH NO_DEFAULT_PATH)
  find_program(RECOMP_FFMPEG_MAKE NAMES make PATHS ENV PATH NO_DEFAULT_PATH)
  if(NOT RECOMP_FFMPEG_SHELL OR NOT RECOMP_FFMPEG_MAKE)
    message(STATUS "RECOMP_VIDEO stays OFF: Windows requires bash and make (MSYS2) on PATH")
  elseif(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    message(STATUS "RECOMP_VIDEO stays OFF: use a MinGW compiler; clang-cl/MSVC FFmpeg builds are out of scope")
  else()
    set(RECOMP_VIDEO_DEFAULT ON)
  endif()
endif()
option(RECOMP_VIDEO "Build the shared FFmpeg Bink and Smacker dependency" ${RECOMP_VIDEO_DEFAULT})
if(WIN32 AND NOT RECOMP_VIDEO_DEFAULT)
  set(RECOMP_VIDEO OFF CACHE BOOL "Build the shared FFmpeg Bink and Smacker dependency" FORCE)
endif()

if(RECOMP_VIDEO)
  if(NOT APPLE AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
    message(FATAL_ERROR "RECOMP_VIDEO is not supported on ${CMAKE_SYSTEM_NAME}")
  endif()
  include(ExternalProject)
  if(NOT WIN32)
    set(RECOMP_FFMPEG_SHELL /bin/sh)
    find_program(RECOMP_FFMPEG_MAKE NAMES make REQUIRED)
  endif()
  set(RECOMP_FFMPEG_PREFIX "${CMAKE_BINARY_DIR}/ffmpeg")
  set(RECOMP_FFMPEG_CONFIGURE
    --prefix=${RECOMP_FFMPEG_PREFIX}
    --enable-shared --disable-static --disable-programs --disable-doc
    --disable-everything --disable-avdevice --disable-avfilter
    --disable-swscale --disable-swresample --disable-postproc --disable-network
    --enable-pic
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct,smacker,smackaud
    --enable-demuxer=bink,smacker --enable-protocol=file
    --disable-autodetect --disable-xlib --disable-libxcb --disable-sdl2
    --disable-iconv --disable-zlib --disable-bzlib --disable-lzma
    --disable-securetransport --disable-audiotoolbox --disable-videotoolbox)
  if(IOS)
    # CMake accepts either an SDK name or an absolute sysroot; FFmpeg needs
    # the directory. This dependency targets arm64 devices, not the simulator.
    set(RECOMP_FFMPEG_SYSROOT "${CMAKE_OSX_SYSROOT}")
    if(NOT IS_DIRECTORY "${RECOMP_FFMPEG_SYSROOT}")
      execute_process(COMMAND xcrun -sdk iphoneos --show-sdk-path
        OUTPUT_VARIABLE RECOMP_FFMPEG_SYSROOT OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    endif()
    list(APPEND RECOMP_FFMPEG_CONFIGURE
      --enable-cross-compile --target-os=darwin --arch=arm64
      "--cc=xcrun -sdk iphoneos clang" --sysroot=${RECOMP_FFMPEG_SYSROOT}
      "--extra-cflags=-arch arm64 -miphoneos-version-min=17.0"
      "--extra-ldflags=-arch arm64 -miphoneos-version-min=17.0"
      --install-name-dir=@rpath)
  elseif(ANDROID)
    # CMake's compiler is inside the selected NDK prebuilt toolchain. Use its
    # API-29 wrapper so configure and every FFmpeg probe target Android 10.
    get_filename_component(RECOMP_FFMPEG_TOOLCHAIN_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(RECOMP_FFMPEG_SYSROOT "${RECOMP_FFMPEG_TOOLCHAIN_BIN}/../sysroot" ABSOLUTE)
    list(APPEND RECOMP_FFMPEG_CONFIGURE
      --enable-cross-compile --target-os=android --arch=aarch64
      --cc=${RECOMP_FFMPEG_TOOLCHAIN_BIN}/aarch64-linux-android29-clang
      --ar=${RECOMP_FFMPEG_TOOLCHAIN_BIN}/llvm-ar
      --nm=${RECOMP_FFMPEG_TOOLCHAIN_BIN}/llvm-nm
      --ranlib=${RECOMP_FFMPEG_TOOLCHAIN_BIN}/llvm-ranlib
      --strip=${RECOMP_FFMPEG_TOOLCHAIN_BIN}/llvm-strip
      --sysroot=${RECOMP_FFMPEG_SYSROOT} --disable-symver)
  elseif(APPLE)
    list(APPEND RECOMP_FFMPEG_CONFIGURE
      --install-name-dir=@rpath --cc=${CMAKE_C_COMPILER})
  else()
    list(APPEND RECOMP_FFMPEG_CONFIGURE --cc=${CMAKE_C_COMPILER})
    if(WIN32)
      list(APPEND RECOMP_FFMPEG_CONFIGURE --target-os=mingw32)
    endif()
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$" OR "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
    list(APPEND RECOMP_FFMPEG_CONFIGURE --disable-x86asm)
  endif()
  set(RECOMP_FFMPEG_LIBRARIES)
  set(RECOMP_FFMPEG_IMPLIBRARIES)
  foreach(component avformat avcodec avutil)
    if(component STREQUAL "avutil")
      set(major 59)
    else()
      set(major 61)
    endif()
    set(libdir lib)
    if(ANDROID)
      # FFmpeg's Android target installs unversioned names and SONAMEs.
      set(filename lib${component}.so)
      set(soname ${filename})
    elseif(APPLE)
      set(filename lib${component}.${major}.dylib)
      set(soname @rpath/${filename})
    elseif(WIN32)
      set(libdir bin)
      set(filename ${component}-${major}.dll)
      set(soname ${filename})
    else()
      set(filename lib${component}.so.${major})
      set(soname ${filename})
    endif()
    list(APPEND RECOMP_FFMPEG_LIBRARIES "${RECOMP_FFMPEG_PREFIX}/${libdir}/${filename}")
    add_library(ffmpeg::${component} SHARED IMPORTED GLOBAL)
    set_target_properties(ffmpeg::${component} PROPERTIES
      IMPORTED_LOCATION "${RECOMP_FFMPEG_PREFIX}/${libdir}/${filename}"
      IMPORTED_SONAME "${soname}"
      INTERFACE_INCLUDE_DIRECTORIES "${RECOMP_FFMPEG_PREFIX}/include")
    if(WIN32)
      set(implib "${RECOMP_FFMPEG_PREFIX}/lib/lib${component}.dll.a")
      set_target_properties(ffmpeg::${component} PROPERTIES IMPORTED_IMPLIB "${implib}")
      list(APPEND RECOMP_FFMPEG_IMPLIBRARIES "${implib}")
    endif()
  endforeach()
  # FFmpeg uses a shell configure script and GNU make, not CMake or Ninja.
  # CMAKE_COMMAND is the same (venv) CMake that configured the kit.
  ExternalProject_Add(ffmpeg
    URL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
    URL_HASH SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND ${RECOMP_FFMPEG_SHELL} <SOURCE_DIR>/configure ${RECOMP_FFMPEG_CONFIGURE}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env ${RECOMP_FFMPEG_MAKE} -j8
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${RECOMP_FFMPEG_MAKE} install
    BUILD_BYPRODUCTS ${RECOMP_FFMPEG_LIBRARIES} ${RECOMP_FFMPEG_IMPLIBRARIES})
  # Imported include paths must exist at generation time, before installation.
  file(MAKE_DIRECTORY "${RECOMP_FFMPEG_PREFIX}/include")
  foreach(component avformat avcodec avutil)
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
