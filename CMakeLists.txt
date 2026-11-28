# NFSU2VITA - Need for Speed Underground 2 (static recompilation) -> PS Vita. ELF -> SELF -> VPK.
cmake_minimum_required(VERSION 3.20)

# vita: the VPK (default). host: the macOS development build (cmake/host.cmake).
set(NFSU_PLATFORM "vita" CACHE STRING "Target: vita or host")
set_property(CACHE NFSU_PLATFORM PROPERTY STRINGS vita host)
if(NFSU_PLATFORM STREQUAL "host")
  project(NFSU2HOST C CXX)
  include(cmake/host.cmake)
  return()
endif()

option(NFSU_RUNTIME_CHECK "Only compile the runtime sources that build without the generated code (CI)" OFF)
# Built with clang and lld only (cmake/vita-clang.cmake, LLVM from NFSU_LLVM), with ThinLTO and PGO.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/vita-clang.cmake" CACHE PATH "toolchain file")
endif()

project(NFSU2VITA C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# GNU mode keeps newlib's POSIX declarations (ftruncate, strcasecmp) visible.
set(CMAKE_CXX_EXTENSIONS ON)

include("${VITASDK}/share/vita.cmake" REQUIRED)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "This project builds with clang only: use the toolchain file cmake/vita-clang.cmake")
endif()

set(VITA_APP_NAME "NFS Underground 2")
set(VITA_TITLEID  "NFSU2REVD")
set(NFSU_VERSION  "0.9.1")
set(VITA_VERSION  "00.91")

option(NFSU_LOG_DEBUG        "Verbose runtime logging" OFF)
option(NFSU_ASSERT_SOFT      "Log failed assertions instead of trapping" OFF)
# Profile-guided optimisation: cmake/clang.cmake.
option(NFSU_PGO_GENERATE "Instrument for a PGO profile" OFF)
set(NFSU_PGO_USE "" CACHE STRING "Profile to optimise with: an indexed .profdata")
option(NFSU_PGO_DEFAULT "Optimise with pgo/nfsu2.proftext when NFSU_PGO_USE is empty" ON)
if(NFSU_PGO_GENERATE AND NFSU_PGO_USE)
  message(FATAL_ERROR "NFSU_PGO_GENERATE and NFSU_PGO_USE exclude each other")
endif()
# The profile names file-local functions by source path, so it is stored with @NFSU_SOURCE_DIR@ in its place.
if(NFSU_PGO_DEFAULT AND NOT NFSU_PGO_GENERATE AND NOT NFSU_PGO_USE
   AND EXISTS "${CMAKE_SOURCE_DIR}/pgo/nfsu2.proftext")
  file(READ "${CMAKE_SOURCE_DIR}/pgo/nfsu2.proftext" NFSU_PGO_TEXT)
  string(REPLACE "@NFSU_SOURCE_DIR@" "${CMAKE_SOURCE_DIR}" NFSU_PGO_TEXT "${NFSU_PGO_TEXT}")
  file(WRITE "${CMAKE_BINARY_DIR}/pgo/nfsu2.proftext" "${NFSU_PGO_TEXT}")
  unset(NFSU_PGO_TEXT)
  execute_process(COMMAND "${NFSU_LLVM_PROFDATA}" merge -o "${CMAKE_BINARY_DIR}/pgo/nfsu2.profdata"
                          "${CMAKE_BINARY_DIR}/pgo/nfsu2.proftext" RESULT_VARIABLE NFSU_PGO_MERGE)
  if(NFSU_PGO_MERGE)
    message(FATAL_ERROR "llvm-profdata could not index pgo/nfsu2.proftext (${NFSU_PGO_MERGE})")
  endif()
  set(NFSU_PGO_USE "${CMAKE_BINARY_DIR}/pgo/nfsu2.profdata")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/pgo/nfsu2.proftext")
endif()

include(cmake/shaders.cmake)
# Diagnostics for development (src/dev: hitch log, mixer counters, benchmark figures, blind driver, scene log).
option(DEV_TOOLS "Development: compile the diagnostics in src/dev (they need that folder)" OFF)
include(cmake/sources.cmake)

nfsu_add_shaders(NFSU_SHADER_SOURCES)

# Native VP6 movies (movies=vp6preferred or vp6only): the minimal FFmpeg that tools/vita/build_ffmpeg_vp6.py builds, linked when present.
set(NFSU_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/build-ffmpeg-vp6/install" CACHE PATH "FFmpeg for VP6 (build_ffmpeg_vp6.py)")
set(NFSU_FFMPEG_LIBS "")
if(EXISTS "${NFSU_FFMPEG_DIR}/lib/libavcodec.a" AND EXISTS "${NFSU_FFMPEG_DIR}/lib/libavutil.a")
  # FFmpeg is built without short enums: this file uses libavcodec's structures directly, and stays out of LTO,
  # which refuses to merge modules with different enum sizes.
  set_source_files_properties(src/ug2/vp6native.cpp PROPERTIES COMPILE_DEFINITIONS NFSU_VP6_NATIVE
                              COMPILE_OPTIONS "-I${NFSU_FFMPEG_DIR}/include;-fno-short-enums;-fno-lto")
  set(NFSU_FFMPEG_LIBS "${NFSU_FFMPEG_DIR}/lib/libavcodec.a" "${NFSU_FFMPEG_DIR}/lib/libavutil.a")
  message(STATUS "Native VP6 movies: FFmpeg from ${NFSU_FFMPEG_DIR}")
else()
  message(STATUS "Native VP6 movies: off (run tools/vita/build_ffmpeg_vp6.py to enable)")
endif()

set(NFSU_ARCH_FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
# Every multiply and add rounded on its own (the VFPv3 unit has no fused multiply-add).
set(NFSU_FP_FLAGS -ffp-contract=off)
set(NFSU_COMMON_FLAGS
  ${NFSU_ARCH_FLAGS}
  ${NFSU_FP_FLAGS}
  # Math functions do not set errno: sqrt is a single vsqrt.f64.
  -fno-math-errno
  -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables
  -ffunction-sections -fdata-sections
  -Wall -Wextra
  # __FILE__ in assertions and logs: paths relative to the repository, nothing from the build machine.
  -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=
)
# Generated code only sees the stable defines; debug switches stay in the runtime to avoid full rebuilds.
option(NFSU_FPU_DOUBLE "x87 values in double (the default is float, as the game runs the x87 at 24-bit precision)" OFF)
if(NFSU_FPU_DOUBLE)
  add_compile_definitions(NFSU_FPU_DOUBLE=1)
endif()
# Each switch fixes its choice at compile time; DEV_SETTINGS lets the settings file decide, key by key.
option(NATIVE_FUNCTIONS "Hand-written native functions in place of their translated originals" ON)
option(AUDIO_FAST_PATHS "Native and NEON routines in the audio mixer" ON)
option(RENDER_FAST_PATHS "Fast paths of the Direct3D layer and the renderer" ON)
option(DEV_SETTINGS "Development: the settings file can switch each of these, without a rebuild" OFF)
set(NFSU_DEFINES VITA __vita__ WITH_MMX)
foreach(switch NATIVE_FUNCTIONS AUDIO_FAST_PATHS RENDER_FAST_PATHS)
  if(${switch})
    list(APPEND NFSU_DEFINES ${switch}=1)
  else()
    list(APPEND NFSU_DEFINES ${switch}=0)
  endif()
endforeach()
if(DEV_SETTINGS)
  list(APPEND NFSU_DEFINES DEV_SETTINGS)
endif()
if(DEV_TOOLS)
  list(APPEND NFSU_DEFINES DEV_TOOLS)
endif()
set(NFSU_RUNTIME_DEFINES ${NFSU_DEFINES} NFSU_VERSION="${NFSU_VERSION}")
# The commit in crash reports (crashlog.cpp), so a report can be matched to its build.
execute_process(COMMAND git rev-parse --short=10 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE NFSU_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NFSU_COMMIT)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_COMMIT="${NFSU_COMMIT}")
endif()
# Offload renderer: where the draw flushes are recorded. 0: inline on the game thread, 1: inside the render thread
# at the splice, 2: a worker on core 2. Empty: off.
# A compile-time choice; no settings key selects the core.
set(NFSU_OFFLOAD_CORE "2" CACHE STRING "Offload renderer producer core: 0 inline, 1 render thread, 2 worker on core 2; empty off")
if(NOT NFSU_OFFLOAD_CORE STREQUAL "")
  if(NOT NFSU_OFFLOAD_CORE MATCHES "^[0-2]$")
    message(FATAL_ERROR "NFSU_OFFLOAD_CORE=${NFSU_OFFLOAD_CORE}: use 0 (game thread), 1 (render thread) or 2 (core 2)")
  endif()
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_OFFLOAD_CORE=${NFSU_OFFLOAD_CORE})
endif()
if(NFSU_LOG_DEBUG)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_LOG_DEBUG)
endif()
if(NFSU_ASSERT_SOFT)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_ASSERT_SOFT)
endif()
# Shadow compare of native functions (src/ug2/native/shadow.h): development builds only, it adds a check per D3D call.
option(NFSU_NATIVE_SHADOW "Shadow compare of native functions (native_shadow=on)" OFF)
if(NFSU_NATIVE_SHADOW)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_NATIVE_SHADOW)
endif()

set(NFSU_GENERATED_DIR "${CMAKE_SOURCE_DIR}/src/ug2/generated" CACHE PATH "Generated game code (tools/recompiler)")
set(NFSU_INCLUDES
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/ug2
  ${NFSU_GENERATED_DIR}
)

# Compiles the runtime sources that do not need the generated game code; no VPK (CI without game binaries).
if(NFSU_RUNTIME_CHECK)
  add_library(nfsu_runtime_check OBJECT ${NFSU_STANDALONE_SOURCES} ${NFSU_SHADER_SOURCES})
  target_compile_options(nfsu_runtime_check PRIVATE ${NFSU_COMMON_FLAGS} -O3 -Werror)
  target_compile_definitions(nfsu_runtime_check PRIVATE ${NFSU_RUNTIME_DEFINES})
  target_include_directories(nfsu_runtime_check PRIVATE ${NFSU_INCLUDES})
  return()
endif()

file(GLOB NFSU_GENERATED_SOURCES CONFIGURE_DEPENDS "${NFSU_GENERATED_DIR}/*.cpp")
if(NOT NFSU_GENERATED_SOURCES)
  message(FATAL_ERROR "${NFSU_GENERATED_DIR} is empty; run tools/build.py (or step 1 of the README) first.")
endif()

include(cmake/clang.cmake)
set(NFSU_NO_SLP -fno-slp-vectorize)

# Generated game code: large, optimise for size and compile time.
add_library(nfsu_game OBJECT ${NFSU_GENERATED_SOURCES})
# No SLP vectorisation: it packs guest stores into NEON stores through the stack (store forwarding stalls).
set(NFSU_GAME_OPT "-O2" CACHE STRING "Optimisation level of the generated code")
target_compile_options(nfsu_game PRIVATE ${NFSU_COMMON_FLAGS} ${NFSU_GAME_OPT} ${NFSU_NO_SLP} -Wno-unused-label -Wno-unused-variable -Wno-unused-parameter ${NFSU_CLANG_GAME_FLAGS})
# Start-up method registration (nfsu.cpp, runs once) gains nothing from -O2 and compiles much faster at -Og.
set(NFSU_LIGHT_SOURCES ${NFSU_GENERATED_SOURCES})
list(FILTER NFSU_LIGHT_SOURCES INCLUDE REGEX "/ug2\\.cpp$")
set_source_files_properties(${NFSU_LIGHT_SOURCES} PROPERTIES COMPILE_OPTIONS -Og)
set_property(SOURCE ${NFSU_LIGHT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${NFSU_CLANG_LIGHT_FLAGS})
target_compile_definitions(nfsu_game PRIVATE ${NFSU_DEFINES})
target_include_directories(nfsu_game PRIVATE ${NFSU_INCLUDES})
# Generated code older than NATIVE_OVERRIDES links without the new natives and no error; this stops the build instead.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_custom_target(nfsu_native_check
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/recompiler/check_natives.py
            ${CMAKE_SOURCE_DIR}/tools/recompiler/games/ug2.py ${NFSU_GENERATED_DIR}
    VERBATIM)
  add_dependencies(nfsu_game nfsu_native_check)
endif()

# Runtime and Vita backend.
add_library(nfsu_runtime OBJECT ${NFSU_RUNTIME_SOURCES} ${NFSU_SHADER_SOURCES})
target_compile_options(nfsu_runtime PRIVATE ${NFSU_COMMON_FLAGS} -O3 ${NFSU_CLANG_FLAGS})
target_compile_definitions(nfsu_runtime PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(nfsu_runtime PRIVATE ${NFSU_INCLUDES})
# The run-time helpers the code generator calls stay out of link-time optimisation, which would drop them.
set_source_files_properties(src/platform/memfuncs.cpp PROPERTIES COMPILE_OPTIONS -fno-lto)
# The kubridge import stubs are top-level assembly: keep them out of link-time optimisation too.
set_source_files_properties(src/platform/kubridge_stub.cpp PROPERTIES COMPILE_OPTIONS -fno-lto)

# LAN play and Online through the NFSU2 Revived server (include/winapi/revived.h).
include(cmake/revived.cmake)
nfsu_revived_libraries(NFSU_REVIVED_LIBRARIES)

add_executable(NFSU2VITA.elf src/ug2/main.cpp tools/shadercomp/shadercomp.cpp ${NFSU_CLANG_SOURCES}
               $<TARGET_OBJECTS:nfsu_game> $<TARGET_OBJECTS:nfsu_runtime>)
target_compile_options(NFSU2VITA.elf PRIVATE ${NFSU_COMMON_FLAGS} -O2 ${NFSU_CLANG_FLAGS})
target_compile_definitions(NFSU2VITA.elf PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(NFSU2VITA.elf PRIVATE ${NFSU_INCLUDES})
# Nothing references the main thread's stack size; keep it from being collected so the loader sees it.
target_link_options(NFSU2VITA.elf PRIVATE -Wl,--gc-sections -Wl,--undefined=sceUserMainThreadStackSize)
# memcpy, memmove and memset go to the small-copy versions in src/platform/memfuncs.cpp.
target_link_options(NFSU2VITA.elf PRIVATE -Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset)
set(NFSU_EXTRA_LINK_FLAGS "" CACHE STRING "Extra linker flags")
if(NFSU_EXTRA_LINK_FLAGS)
  target_link_options(NFSU2VITA.elf PRIVATE ${NFSU_EXTRA_LINK_FLAGS})
endif()
target_link_options(NFSU2VITA.elf PRIVATE ${NFSU_CLANG_LINK_FLAGS})
target_link_libraries(NFSU2VITA.elf ${NFSU_CLANG_LIBRARIES})
set_target_properties(NFSU2VITA.elf PROPERTIES LINK_DEPENDS "${NFSU_LLD_SCRIPT}")
target_link_libraries(NFSU2VITA.elf
  ${NFSU_REVIVED_LIBRARIES}
  stdc++
  ${NFSU_FFMPEG_LIBS}
  SceShaccCg_stub_weak
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceAudio_stub
  SceAudiodec_stub
  SceVideodec_stub
  SceSysmodule_stub
  SceNet_stub
  SceNetCtl_stub
  ScePower_stub
  SceRtc_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  SceProcessmgr_stub
  pthread
  c
  m
)

vita_create_self(NFSU2VITA.self NFSU2VITA.elf UNSAFE)

# The launcher (src/launcher): the start-up screen and settings manager, then the game as a fresh process. With
# NFSU_LAUNCHER the VPK ships it as eboot.bin and the game as game.bin; NFSU2VITA.self alone is still the whole game.
option(NFSU_LAUNCHER "VPK: the launcher as eboot.bin, the game as game.bin" ON)
add_executable(NFSU2LAUNCH.elf
  src/launcher/main.cpp src/launcher/cfgui.cpp src/launcher/cfgui_draw.cpp src/launcher/cfgui_assets.cpp
  src/launcher/cfgui_sysfont.cpp src/launcher/mods_edit.cpp src/launcher/save.cpp src/launcher/log.cpp
  src/launcher/mods_registry.cpp src/ug2/mods_require.cpp src/ug2/mods_hook.cpp src/ug2/notice_text.cpp
  src/ug2/language.cpp src/platform/regfile.cpp
  src/platform/settings.cpp src/platform/cfgfile.cpp src/platform/textfile.cpp)
# Start-up screen shots on request (shot_c.req) with the game's screenshot writer, development builds only.
if(DEV_TOOLS)
  target_sources(NFSU2LAUNCH.elf PRIVATE src/launcher/shots.cpp src/dev/screenshot.cpp)
endif()
target_compile_options(NFSU2LAUNCH.elf PRIVATE ${NFSU_COMMON_FLAGS} -O2)
target_compile_definitions(NFSU2LAUNCH.elf PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(NFSU2LAUNCH.elf PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/ug2
                           ${CMAKE_SOURCE_DIR}/src/launcher)
target_link_options(NFSU2LAUNCH.elf PRIVATE -Wl,--gc-sections "-Wl,-T,${NFSU_LLD_SCRIPT}")
set_target_properties(NFSU2LAUNCH.elf PROPERTIES LINK_DEPENDS "${NFSU_LLD_SCRIPT}")
target_link_libraries(NFSU2LAUNCH.elf
  stdc++
  SceAppMgr_stub
  SceCtrl_stub
  SceDisplay_stub
  SceIofilemgr_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  ScePgf_stub
  ScePower_stub
  SceProcessmgr_stub
  SceSysmem_stub
  SceSysmodule_stub
  SceTouch_stub
  c
  m
)
# Mod downloads, launcher only: VitaSDK packages from tools/deps.py (curl, mbedTLS, zlib, zstd, liblzma, bzip2) and
# libarchive from tools/vita/build_libarchive.py. Without them, no downloads.
set(NFSU_VITA_PACKAGES "" CACHE PATH "Folder with arm-vita-eabi/include and lib of the launcher's libraries")
set(NFSU_LIBARCHIVE_DIR "${CMAKE_SOURCE_DIR}/build-libarchive/install" CACHE PATH "libarchive (build_libarchive.py)")
set(NFSU_VPK_LAUNCHER_FILES "")
set(NFSU_PACKAGES_LIB "${NFSU_VITA_PACKAGES}/arm-vita-eabi/lib")
if(NFSU_VITA_PACKAGES AND EXISTS "${NFSU_PACKAGES_LIB}/libcurl.a" AND EXISTS "${NFSU_PACKAGES_LIB}/liblzma.a"
   AND EXISTS "${NFSU_LIBARCHIVE_DIR}/lib/libarchive.a")
  target_sources(NFSU2LAUNCH.elf PRIVATE src/launcher/mods_download.cpp src/launcher/net_curl.cpp
                 src/launcher/unpack.cpp src/launcher/file_lock.cpp src/platform/sha256.cpp)
  target_compile_definitions(NFSU2LAUNCH.elf PRIVATE NFSU_MOD_DOWNLOAD)
  target_include_directories(NFSU2LAUNCH.elf PRIVATE "${NFSU_LIBARCHIVE_DIR}/include"
                             "${NFSU_VITA_PACKAGES}/arm-vita-eabi/include")
  target_link_libraries(NFSU2LAUNCH.elf
    "${NFSU_LIBARCHIVE_DIR}/lib/libarchive.a"
    "${NFSU_PACKAGES_LIB}/libcurl.a"
    "${NFSU_PACKAGES_LIB}/libmbedtls.a"
    "${NFSU_PACKAGES_LIB}/libmbedx509.a"
    "${NFSU_PACKAGES_LIB}/libmbedcrypto.a"
    "${NFSU_PACKAGES_LIB}/libeverest.a"
    "${NFSU_PACKAGES_LIB}/libp256m.a"
    "${NFSU_PACKAGES_LIB}/liblzma.a"
    "${NFSU_PACKAGES_LIB}/libbz2.a"
    "${NFSU_PACKAGES_LIB}/libzstd.a"
    "${NFSU_PACKAGES_LIB}/libz.a"
    pthread
    SceNet_stub
    SceNetCtl_stub
    SceShellSvc_stub
  )
  set(NFSU_VPK_LAUNCHER_FILES -a "${CMAKE_SOURCE_DIR}/src/launcher/cacert.pem=launcher/cacert.pem"
      -a "${CMAKE_SOURCE_DIR}/src/launcher/LICENSES.txt=launcher/LICENSES.txt")
  message(STATUS "Mod downloads: on (libarchive from ${NFSU_LIBARCHIVE_DIR})")
else()
  message(STATUS "Mod downloads: off (set NFSU_VITA_PACKAGES and run tools/vita/build_libarchive.py to enable)")
endif()
vita_create_self(NFSU2LAUNCH.self NFSU2LAUNCH.elf UNSAFE)

configure_file(
  ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml.in
  ${CMAKE_BINARY_DIR}/generated/livearea/template.xml
  @ONLY NEWLINE_STYLE LF)

# Images made from the user's game files (tools/vita/livearea_art.py) replace the plain defaults.
set(NFSU_ART_DIR "${CMAKE_SOURCE_DIR}/sce_sys")
if(NOT NFSU_PLAIN_ART AND EXISTS "${NFSU_ART_DIR}/game/icon0.png" AND EXISTS "${NFSU_ART_DIR}/game/livearea/contents/bg.png"
   AND EXISTS "${NFSU_ART_DIR}/game/livearea/contents/startup.png")
  set(NFSU_ART_DIR "${NFSU_ART_DIR}/game")
endif()
message(STATUS "Icon and LiveArea images: ${NFSU_ART_DIR}")

# Packaged here, not with vita_create_vpk: that macro breaks on folders with spaces in their names.
set(NFSU_SELF "${CMAKE_BINARY_DIR}/NFSU2VITA.self")
set(NFSU_LAUNCH_SELF "${CMAKE_BINARY_DIR}/NFSU2LAUNCH.self")
if(NFSU_LAUNCHER)
  set(NFSU_VPK_EBOOT "${NFSU_LAUNCH_SELF}")
  set(NFSU_VPK_GAME -a "${NFSU_SELF}=game.bin")
else()
  set(NFSU_VPK_EBOOT "${NFSU_SELF}")
  set(NFSU_VPK_GAME "")
endif()
set(NFSU_SFO "${CMAKE_BINARY_DIR}/NFSU2VITA.vpk_param.sfo")
set(NFSU_VPK "${CMAKE_BINARY_DIR}/NFSU2VITA.vpk")
set(NFSU_TEMPLATE "${CMAKE_BINARY_DIR}/generated/livearea/template.xml")
# ATTRIBUTE2=12: extended memory budget for the guest arena.
add_custom_command(OUTPUT "${NFSU_SFO}"
  COMMAND "${VITA_MKSFOEX}" -d ATTRIBUTE2=12 -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID}
          "${VITA_APP_NAME}" "${NFSU_SFO}"
  DEPENDS "${NFSU_SELF}"
  COMMENT "Generating param.sfo"
  VERBATIM)
add_custom_command(OUTPUT "${NFSU_VPK}"
  COMMAND "${VITA_PACK_VPK}"
          -a "${NFSU_ART_DIR}/icon0.png=sce_sys/icon0.png"
          -a "${NFSU_ART_DIR}/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png"
          -a "${NFSU_ART_DIR}/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png"
          -a "${NFSU_TEMPLATE}=sce_sys/livearea/contents/template.xml"
          ${NFSU_VPK_GAME}
          ${NFSU_VPK_LAUNCHER_FILES}
          -s "${NFSU_SFO}" -b "${NFSU_VPK_EBOOT}" "${NFSU_VPK}"
  DEPENDS "${NFSU_SFO}" "${NFSU_SELF}" "${NFSU_LAUNCH_SELF}" "${NFSU_TEMPLATE}" "${NFSU_ART_DIR}/icon0.png"
          "${NFSU_ART_DIR}/livearea/contents/bg.png" "${NFSU_ART_DIR}/livearea/contents/startup.png"
  COMMENT "Building NFSU2VITA.vpk"
  VERBATIM)
add_custom_target(NFSU2VITA.vpk-vpk ALL DEPENDS "${NFSU_VPK}")
add_dependencies(NFSU2VITA.vpk-vpk NFSU2VITA.self-self NFSU2LAUNCH.self-self)
