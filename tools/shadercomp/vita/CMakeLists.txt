# NFSU2VITA.vpk holding only the shader compile mode, for the first install before the game builds.
#   cmake -S tools/shadercomp/vita -B build-shadercomp -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()
project(NFSU2VITA_SHADERCOMP CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

add_executable(shadercomp.elf main.cpp ../shadercomp.cpp)
target_compile_options(shadercomp.elf PRIVATE -O2 -Wall -Wextra)
target_link_libraries(shadercomp.elf
  SceShaccCg_stub_weak
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  SceLibKernel_stub
  c
)

vita_create_self(eboot.bin shadercomp.elf UNSAFE)
set(NFSU_VERSION "0.1")
configure_file(${REPO}/sce_sys/livearea/contents/template.xml.in ${CMAKE_BINARY_DIR}/livearea/template.xml @ONLY)
vita_create_vpk(NFSU2VITA.vpk NFSU2REVD eboot.bin
  VERSION 00.01
  NAME "NFS Underground 2"
  FILE ${REPO}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${REPO}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${REPO}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_BINARY_DIR}/livearea/template.xml sce_sys/livearea/contents/template.xml
)
