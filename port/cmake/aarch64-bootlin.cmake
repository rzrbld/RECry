# aarch64 cross build with a Bootlin glibc toolchain.
# RECry ships builds made with aarch64--glibc--stable-2020.02-2 (GCC 8.4,
# glibc 2.30) so they run on older PortMaster firmware too.
#   -DTOOLCHAIN_ROOT=/path/to/aarch64--glibc--stable-2020.02-2
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
if(NOT TOOLCHAIN_ROOT)
  set(TOOLCHAIN_ROOT "$ENV{TOOLCHAIN_ROOT}")
endif()
set(TOOLCHAIN_ROOT "${TOOLCHAIN_ROOT}" CACHE PATH "Bootlin aarch64 glibc toolchain root")
set(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-buildroot-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-buildroot-linux-gnu-g++")
set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_ROOT}/aarch64-buildroot-linux-gnu/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
