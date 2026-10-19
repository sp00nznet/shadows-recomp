# =============================================================================
# Star Wars: Shadows of the Empire - static recompilation host (32-bit, MSVC x86)
#
#   py -3 run_lift.py --all  # -> src/recomp/gen/  (needs work/functions.json)
#   build.cmd                # vcvarsall x86 + CMake + Ninja -> build/sote.exe
#   build\sote.exe --run --watchdog 60
# =============================================================================
cmake_minimum_required(VERSION 3.20)
# The C runtime linked in (/MT): sote.exe runs on a PC without the x86 Visual
# C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(sote C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall x86)")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()

# The toolkit: this checkout's own worktree (work/pcrecomp, README), else a
# sibling ../tools, which is where netlab puts the DEPS=pcrecomp=tools copy.
if(EXISTS "${CMAKE_SOURCE_DIR}/work/pcrecomp/runtime")
  set(_PCRECOMP_DEFAULT "${CMAKE_SOURCE_DIR}/work/pcrecomp")
else()
  set(_PCRECOMP_DEFAULT "${CMAKE_SOURCE_DIR}/../tools")
endif()
set(PCRECOMP "${_PCRECOMP_DEFAULT}" CACHE PATH "pcrecomp checkout")
set(GEN_DIR src/recomp/gen)
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN_DIR} is empty -- run `py -3 run_lift.py --all` first")
endif()

set(HOST_SOURCES src/runtime/host.c src/runtime/display.c src/runtime/pad.c src/runtime/menu.c src/runtime/oracle.c)
add_executable(sote
  ${HOST_SOURCES}
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(sote PRIVATE
  src/runtime ${GEN_DIR} ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(sote PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                     RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so the guest's 0x00400000..0x008F6000 is free.
target_link_options(sote PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                /NXCOMPAT /LARGEADDRESSAWARE /STACK:0x1000000
                                /MANIFESTUAC:NO)
set(GEN_OPT "/O2" CACHE STRING "optimisation for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3")
# No ddraw/dinput import here: the guest's DDRAW.dll must be the Steam
# wrapper in Sdata\, and a host import would load the system one first.
target_link_libraries(sote PRIVATE user32 gdi32 kernel32 advapi32 winmm)

option(SOTE_TRACE "record a ring buffer of lifted function entries" OFF)
if(SOTE_TRACE)
  target_compile_definitions(sote PRIVATE RECOMP_TRACE=1)
endif()
