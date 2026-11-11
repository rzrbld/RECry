cmake_minimum_required(VERSION 3.20)
project(RECry C CXX)
# RECry: Far Cry (CryEngine 1) for Linux / OpenGL ES 3 handhelds (PortMaster).
# One executable, aarch64, rendering through the native GLES3 renderer and SDL2.
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=port/cmake/aarch64-bootlin.cmake \
#         -DTOOLCHAIN_ROOT=<bootlin toolchain> -DFFMPEG_ROOT=<ffmpeg out> -DSDL2_LIB=<libSDL2-2.0.so.0>
#
# See BUILDING.md.

set(FFMPEG_ROOT "" CACHE PATH "Minimal FFmpeg build (include/, lib/) for the Bink cutscenes")
set(SDL2_LIB "" CACHE FILEPATH "aarch64 libSDL2-2.0.so.0 to link against (the device provides SDL2 at run time)")
if(NOT FFMPEG_ROOT OR NOT SDL2_LIB)
  message(FATAL_ERROR "Set FFMPEG_ROOT and SDL2_LIB (see BUILDING.md)")
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# -fno-lifetime-dse: engine classes zero themselves in a class operator new
# (malloc + memset); GCC >= 6 otherwise deletes that memset as a dead store.
# Binaries must not embed build-machine paths (__FILE__ in asserts/logs).
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-lifetime-dse -fpermissive -fno-strict-aliasing -Wno-narrowing -Wno-deprecated-declarations -Wno-write-strings -Wno-register -Wno-format-security")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-strict-aliasing")
# The engine was written for MSVC/x86 where char is signed; ARM defaults to
# unsigned char (save games broke). Use signed char everywhere.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsigned-char")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsigned-char")

add_definitions(-DLINUX -DLINUX64 -DNO_ASM -DNOT_USE_DIVX_SDK -DCRY_PORT_UNIX -DCRY_STATIC_BUILD -DCRY_PORT_NO_NETWORK -DFARCRY_PORT_BINK)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR})
set(PORT ${SRC}/port)
include_directories(
  ${PORT}/shim
  ${PORT}/inc_all
  ${SRC}
  ${SRC}/CrySystem/zlib
  ${SRC}/CrySystem/XML/Expat
  ${SRC}/CryScriptSystem/LUA
  ${SRC}/RenderDll/Common
  ${SRC}/RenderDll/Common/Shaders
  ${SRC}/RenderDll
  ${SRC}/RenderDll/XRenderOGL
)
include_directories(BEFORE ${SRC}/CryCommon ${SRC}/CrySystem ${SRC}/RenderDll ${SRC}/RenderDll/Common)

# ---------------- FreeType 2.1.9 (part of the engine source) ----------------
file(GLOB_RECURSE FT_SRC
  ${SRC}/CryFont/FreeType2/src/base/*.c
  ${SRC}/CryFont/FreeType2/src/autohint/*.c
  ${SRC}/CryFont/FreeType2/src/psaux/*.c
  ${SRC}/CryFont/FreeType2/src/psnames/*.c
  ${SRC}/CryFont/FreeType2/src/pshinter/*.c
  ${SRC}/CryFont/FreeType2/src/raster/*.c
  ${SRC}/CryFont/FreeType2/src/sfnt/*.c
  ${SRC}/CryFont/FreeType2/src/smooth/*.c
  ${SRC}/CryFont/FreeType2/src/truetype/*.c
  ${SRC}/CryFont/FreeType2/src/winfonts/*.c
)
list(FILTER FT_SRC EXCLUDE REGEX "/(ftexcept|ftmac|ftbdf|ftsysio|ftsysmem)\\.c$")
list(FILTER FT_SRC EXCLUDE REGEX "/(base/ftbase|winfonts/winfnt|pshinter/pshinter|psnames/psnames|psaux/psaux|autohint/autohint|truetype/truetype|sfnt/sfnt|smooth/smooth|raster/raster)\\.c$")
add_library(freetype STATIC ${FT_SRC})
target_include_directories(freetype PUBLIC ${SRC}/CryFont/FreeType2/include)
target_compile_definitions(freetype PUBLIC FT2_BUILD_LIBRARY)

# ---------------- zlib / expat / lua / md5 (part of the engine source) ----------------
file(GLOB ZLIB_SRC ${SRC}/CrySystem/zlib/*.c)
file(GLOB EXPAT_SRC ${SRC}/CrySystem/XML/Expat/*.c)
list(FILTER EXPAT_SRC EXCLUDE REGEX "xmltok_(impl|ns)\\.c$")
add_library(cry_zlib STATIC ${ZLIB_SRC})
target_compile_definitions(cry_zlib PRIVATE HAVE_UNISTD_H)
add_library(cry_expat STATIC ${EXPAT_SRC})
add_library(cry_md5 STATIC ${SRC}/CrySystem/md5.c)
file(GLOB LUA_SRC ${SRC}/CryScriptSystem/LUA/*.c ${SRC}/CryScriptSystem/LUA/lib/*.c ${SRC}/CryScriptSystem/vectorlib.c)
list(FILTER LUA_SRC EXCLUDE REGEX "ltests\\.c$")
add_library(cry_lua STATIC ${LUA_SRC})
target_compile_definitions(cry_lua PRIVATE _CRT_SECURE_NO_WARNINGS)

# ---------------- engine modules ----------------
file(GLOB CRYSYSTEM_SRC ${SRC}/CrySystem/*.cpp ${SRC}/CrySystem/XML/*.cpp)
list(FILTER CRYSYSTEM_SRC EXCLUDE REGEX "(^|/)(SystemWin32|SourceSafeHelper|DownloadManager|HTTPDownloader|Mailer)\\.cpp$")
file(GLOB CRYGAME_SRC ${SRC}/CryGame/*.cpp)
file(GLOB CRYMOVIE_SRC ${SRC}/CryMovie/*.cpp)
file(GLOB CRY3DENGINE_SRC ${SRC}/Cry3DEngine/*.cpp)
file(GLOB CRYENTITY_SRC ${SRC}/CryEntitySystem/*.cpp)
file(GLOB CRYANIM_SRC ${SRC}/CryAnimation/*.cpp)
file(GLOB CRYPHYS_SRC ${SRC}/CryPhysics/*.cpp)
# Physics keeps its original branchless min/max (see CryCommon/Cry_Math.h).
set_source_files_properties(${CRYPHYS_SRC} PROPERTIES COMPILE_DEFINITIONS RECRY_PHYSICS_MINMAX)
file(GLOB CRYAI_SRC ${SRC}/CryAISystem/*.cpp)
file(GLOB CRYFONT_SRC ${SRC}/CryFont/*.cpp)
list(FILTER CRYFONT_SRC EXCLUDE REGEX "/CryPakIO\\.cpp$")
file(GLOB CRYSCRIPT_SRC ${SRC}/CryScriptSystem/*.cpp)
set(CRYINPUT_SRC ${SRC}/CryInput/CryInput.cpp ${SRC}/CryInput/Input.cpp ${SRC}/CryInput/XActionMap.cpp ${SRC}/CryInput/XActionMapManager.cpp ${SRC}/CryInput/SDLDevices.cpp)
set(CRYNETWORK_SRC CryNetwork Network Server Client ClientLocal ServerSlot ClientStateMachine ServerStateMachine RConSystem
    ServerSnooper NETServerSnooper DatagramSocket CompressionHelper StaticCharCompressor DefenceWall CCPEndpoint CTPEndpoint CTPEndpointGNB)
list(TRANSFORM CRYNETWORK_SRC PREPEND ${SRC}/CryNetwork/)
list(TRANSFORM CRYNETWORK_SRC APPEND .cpp)

# ---------------- renderer: shared render/shader code + native GLES3 ----------------
file(GLOB_RECURSE RENDERCOMMON_SRC ${SRC}/RenderDll/Common/*.cpp)
list(FILTER RENDERCOMMON_SRC EXCLUDE REGEX "/XRenderD3D|/Shaders/cg|/NVParse")
file(GLOB RENDERGLES3_SRC ${SRC}/RenderDll/XRenderGLES3/*.cpp)
list(FILTER RENDERGLES3_SRC EXCLUDE REGEX "(GL_OutSpace|GL_Combiners|state_to_nvparse_text)\\.cpp$")
# etcpak (BSD-3) encodes ETC2 for GPUs without DXT/S3TC.
set(ETCPAK_DIR ${SRC}/third_party/etcpak)
set(ETCPAK_SRC ${ETCPAK_DIR}/ProcessRGB.cpp ${ETCPAK_DIR}/Tables.cpp ${ETCPAK_DIR}/Dither.cpp)
add_library(recry_gles3 OBJECT ${RENDERCOMMON_SRC} ${RENDERGLES3_SRC} ${ETCPAK_SRC})
set_source_files_properties(${ETCPAK_SRC} PROPERTIES COMPILE_OPTIONS "-O3;-w")
target_compile_definitions(recry_gles3 PRIVATE OPENGL _RENDERER GLES3_RENDERER FARCRY_GLES3 NOT_USE_UBICOM_SDK NOT_USE_PUNKBUSTER_SDK)
target_include_directories(recry_gles3 PRIVATE ${ETCPAK_DIR} ${PORT}/gles3_compat ${SRC}/RenderDll/XRenderGLES3 ${SRC}/CryFont/FreeType2/include ${PORT}/third_party/SDL2 ${PORT}/third_party)

# ---------------- the game ----------------
add_executable(farcry
  ${PORT}/main/MainLinux.cpp
  ${PORT}/platform/SystemUnix.cpp
  ${PORT}/platform/PortSampler.cpp
  ${PORT}/platform/HTTPDownloaderStub.cpp
  ${PORT}/platform/SDL2Sound.cpp
  ${PORT}/platform/BinkPort.cpp                 # Bink cutscenes through FFmpeg (LGPL, shared)
  ${PORT}/third_party/stb/stb_vorbis.c
  ${SRC}/CrySystem/DownloadManager.cpp
  ${CRYSYSTEM_SRC} ${CRYGAME_SRC} ${CRYMOVIE_SRC} ${CRY3DENGINE_SRC} ${CRYENTITY_SRC}
  ${CRYANIM_SRC} ${CRYPHYS_SRC} ${CRYAI_SRC} ${CRYFONT_SRC} ${CRYSCRIPT_SRC}
  ${CRYINPUT_SRC} ${CRYNETWORK_SRC} $<TARGET_OBJECTS:recry_gles3>
)
target_compile_definitions(farcry PRIVATE FARCRY_GLES3 NOT_USE_UBICOM_SDK NOT_USE_PUNKBUSTER_SDK)
target_include_directories(farcry PRIVATE ${SRC}/CryFont/FreeType2/include ${PORT}/third_party/SDL2 ${PORT}/third_party ${PORT}/compat ${FFMPEG_ROOT}/include)
# GL/GLES, EGL and the display come from the device through SDL at run time;
# only SDL2 and the bundled FFmpeg are linked. libstdc++/libgcc are static so
# the binary needs nothing newer than the toolchain's glibc.
target_link_libraries(farcry PRIVATE freetype cry_zlib cry_expat cry_lua cry_md5
  ${FFMPEG_ROOT}/lib/libavformat.so ${FFMPEG_ROOT}/lib/libavcodec.so ${FFMPEG_ROOT}/lib/libswresample.so ${FFMPEG_ROOT}/lib/libavutil.so
  ${SDL2_LIB} dl pthread rt m)
set_target_properties(farcry PROPERTIES SKIP_BUILD_RPATH ON)
target_link_options(farcry PRIVATE -static-libstdc++ -static-libgcc -Wl,--allow-shlib-undefined
  "-Wl,-rpath,$ORIGIN/libs.aarch64" "-Wl,-rpath-link,${FFMPEG_ROOT}/lib")
