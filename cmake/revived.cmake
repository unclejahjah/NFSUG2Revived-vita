# The NFSU2 Revived link (include/winapi/revived.h): nfsu2-web's Winsock layer (src/lib/winapi/revived, C), the
# WebSocket link to the server, and the TLS it runs on (Mbed TLS, src/platform/tls.cpp).
#
# nfsu_revived_libraries(<out-var>): adds the static libraries nfsu_mbedtls and nfsu_revived and returns their names.
# Mbed TLS comes from NFSU_MBEDTLS (a source tree), else its release archive is downloaded once into the build
# directory and checked against its SHA-256.

set(NFSU_MBEDTLS "$ENV{NFSU_MBEDTLS}" CACHE PATH "Mbed TLS 3.6 source tree (empty: download 3.6.2)")

function(nfsu_revived_libraries out_var)
  set(version 3.6.2)
  if(NFSU_MBEDTLS)
    set(root "${NFSU_MBEDTLS}")
  else()
    set(root "${CMAKE_BINARY_DIR}/mbedtls-${version}")
    if(NOT EXISTS "${root}/library/ssl_tls.c")
      set(archive "${CMAKE_BINARY_DIR}/mbedtls-${version}.tar.bz2")
      set(url "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${version}/mbedtls-${version}.tar.bz2")
      message(STATUS "Revived link: downloading Mbed TLS ${version}")
      file(DOWNLOAD "${url}" "${archive}" TLS_VERIFY ON STATUS status
           EXPECTED_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca)
      list(GET status 0 code)
      if(NOT code EQUAL 0)
        file(REMOVE "${archive}")
        message(FATAL_ERROR "Download of ${url} failed (${status}); set NFSU_MBEDTLS")
      endif()
      file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}")
      file(REMOVE "${archive}")
    endif()
  endif()
  file(GLOB mbedtls_sources "${root}/library/*.c")
  if(NOT mbedtls_sources)
    message(FATAL_ERROR "${root}/library has no sources (NFSU_MBEDTLS)")
  endif()
  set(config_dir "${CMAKE_SOURCE_DIR}/src/platform/tls")
  add_library(nfsu_mbedtls STATIC ${mbedtls_sources})
  target_compile_options(nfsu_mbedtls PRIVATE ${NFSU_ARCH_FLAGS} -O2 -ffunction-sections -fdata-sections -w)
  target_compile_definitions(nfsu_mbedtls PUBLIC "MBEDTLS_USER_CONFIG_FILE=<mbedtls_vita_config.h>")
  target_include_directories(nfsu_mbedtls PUBLIC "${root}/include" "${config_dir}" PRIVATE "${root}/library")

  # Mozilla's CA certificates, as a C array
  set(cacert "${CMAKE_SOURCE_DIR}/third_party/cacert/cacert.pem")
  set(cacert_c "${CMAKE_BINARY_DIR}/generated/tls_cacert.c")
  add_custom_command(
    OUTPUT "${cacert_c}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${cacert}" "-DOUTPUT=${cacert_c}" "-DSYMBOL=tls_cacert_pem"
            -P "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    DEPENDS "${cacert}" "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    COMMENT "Embedding the CA certificates"
    VERBATIM)

  set(dir src/lib/winapi/revived)
  set(c_sources ${dir}/winsock.c ${dir}/net_lan.c)
  # free roam (src/ug2/freeroam.cpp): nfsu2-web's mod as its PC plugin builds it (-DROAM_HOST_WIN), on the
  # browser's network (roam_web.c)
  # ...and its multiplayer races from free roam (roam_game.c mp_*, ROAM_MPRACES): nfsu2-web's mprace.c, the trip through
  # the game's own LAN race, with what it needs of the browser's runtime (mp/mp_vita.c); hosting only with the
  # translated server.dll next to SPEED2.EXE (ROAM_MPRACE_HOST_CHECK, src/lib/guestmodule.cpp), prize money only (no career credit: ROAM_MPRACE_NO_CREDIT)
  set(roam_sources ${dir}/roam/roam_game.c ${dir}/roam/roam_core.c ${dir}/roam/roam_web.c
                   ${dir}/roam/mp/mprace.c ${dir}/roam/mp/mp_vita.c)
  # horns between players and friends' engines, nitrous and hydraulics, mixed over the game's sound (nfsu2-web's
  # horn.c and carsfx.c, with the Vita's host: roam_vita.cpp, src/lib/audio.cpp)
  set(sound_sources ${dir}/roam/horn.c ${dir}/roam/carsfx.c)
  # ...and the show-off (the hood, the trunk, the doors) and stance (nfsu2-web's showoff.c and stance.c's drawing; the
  # natives into the game's car drawing: src/ug2/showoff_hooks.cpp)
  set(showoff_sources ${dir}/roam/showoff.c ${dir}/roam/stance_vita.c)
  # ...and drift mode and drift battles (nfsu2-web's drift.c: drift_button, natives in src/ug2/showoff_hooks.cpp)
  set(drift_sources ${dir}/roam/drift.c)
  add_library(nfsu_revived STATIC
    ${c_sources}
    ${roam_sources}
    ${sound_sources}
    ${showoff_sources}
    ${drift_sources}
    ${dir}/roam/roam_vita.cpp
    ${dir}/revived_bridge.cpp
    ${dir}/revived_link.cpp
    src/platform/tls.cpp
    src/platform/cloud_saves.cpp
    "${cacert_c}")
  target_compile_options(nfsu_revived PRIVATE ${NFSU_COMMON_FLAGS} -O2)
  target_compile_definitions(nfsu_revived PRIVATE ${NFSU_RUNTIME_DEFINES})
  target_include_directories(nfsu_revived PRIVATE ${NFSU_INCLUDES} "${CMAKE_SOURCE_DIR}/${dir}")
  target_link_libraries(nfsu_revived PRIVATE nfsu_mbedtls)
  # nfsu2-web's own code as it is (its warnings are its own); the browser backend of its virtual LAN
  set_source_files_properties(${c_sources} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS NFSU2_VLAN_BACKEND)
  set_source_files_properties(${roam_sources} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS "ROAM_HOST_WIN;ROAM_GROUP_CAP;ROAM_FLAGS;ROAM_SAYS;ROAM_SAY_TAGS;ROAM_DUELS;ROAM_VITA_SOUNDS;ROAM_VITA_SHOWOFF;ROAM_VITA_DRIFT;ROAM_LOBBY_PANEL;ROAM_MPRACES;ROAM_MPRACE_HOST_CHECK;ROAM_MPRACE_PLAYER_SAVES;ROAM_MPRACE_NO_CREDIT;ROAM_PROBE;ROAM_STREAM_REARM;ROAM_SPREAD_CREATE;ROAM_LOADS_AHEAD=1;ROAM_PREFETCH;ROAM_SWAP_CALM;HEAVY_PER_FRAME=1")
  set_source_files_properties(${sound_sources} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS "HORN_VITA;CARSFX_VITA")
  set_source_files_properties(${showoff_sources} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS "SHOWOFF_VITA")
  set_source_files_properties(${drift_sources} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS "DRIFT_VITA")
  set(${out_var} nfsu_revived nfsu_mbedtls PARENT_SCOPE)
endfunction()
