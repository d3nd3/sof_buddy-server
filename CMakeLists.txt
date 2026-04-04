cmake_minimum_required(VERSION 3.16)

project(sof_buddy_gamex86
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# For clangd / IDE tooling (written to the build dir; harmless with Ninja).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")

file(GLOB_RECURSE SOF_SRV_CFG_JSON CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/**/*.json"
)

add_custom_command(
  OUTPUT
    "${GEN_DIR}/generated_detours.h"
    "${GEN_DIR}/generated_detours.cpp"
    "${GEN_DIR}/generated_registrations.h"
    "${GEN_DIR}/generated_engine_pointers.h"
    "${GEN_DIR}/generated_engine_pointers.cpp"
  COMMAND ${CMAKE_COMMAND} -E env "SOF_BUDDY_SERVER_GEN_DIR=${GEN_DIR}"
          ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/generate_hooks.py"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/generate_hooks.py"
    "${CMAKE_SOURCE_DIR}/detours.yaml"
    "${CMAKE_SOURCE_DIR}/features/features.yaml"
    ${SOF_SRV_CFG_JSON}
  COMMENT "Generate detours from detours.yaml and feature hooks/callbacks JSON"
)

add_library(gamex86 SHARED
  src/gamex86.cpp
  "${GEN_DIR}/generated_detours.cpp"
  "${GEN_DIR}/generated_engine_pointers.cpp"
  src/runtime/log.cpp
  src/runtime/detours.cpp
  src/runtime/shared_hook_manager.cpp
  third_party/DetourXS/ADE32.cpp
  third_party/DetourXS/detourxs.cpp
  src/features/example/example.cpp
  src/features/ctf_spawn/ctf_spawn.cpp
)

set_source_files_properties(
  "${GEN_DIR}/generated_detours.cpp"
  "${GEN_DIR}/generated_engine_pointers.cpp"
  PROPERTIES GENERATED TRUE
)

target_include_directories(gamex86 PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/third_party"
  "${GEN_DIR}"
)

set_target_properties(gamex86 PROPERTIES
  PREFIX ""
  OUTPUT_NAME "gamex86"
)

# Fully static MinGW runtime + winpthread so Base/ needs only gamex86.dll (no libwinpthread-1.dll).
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_target_properties(gamex86 PROPERTIES
      LINK_FLAGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gamex86.def -static-libgcc -static-libstdc++ -static"
    )
  endif()
endif()
