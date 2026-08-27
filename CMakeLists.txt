cmake_minimum_required(VERSION 3.16)
project(sof_buddy-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}")

set(GENERATED_SOURCES
    ${GENERATED_DIR}/generated_detours.cpp
    ${GENERATED_DIR}/generated_engine_pointers.cpp
)

set(GENERATED_HEADERS
    ${GENERATED_DIR}/generated_detours.h
    ${GENERATED_DIR}/generated_registrations.h
    ${GENERATED_DIR}/generated_engine_pointers.h
)

file(GLOB_RECURSE HOOK_JSON_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/features/*/hooks/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/features/*/callbacks/*.json"
)

add_custom_command(
    OUTPUT ${GENERATED_SOURCES} ${GENERATED_HEADERS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_hooks.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/detours.yaml
        ${HOOK_JSON_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_hooks.py
    COMMENT "Generate detours from detours.yaml and feature hooks/callbacks JSON"
)

# Compile only the features src/features/features.yaml turns on.
#
# The generator honours that file when deciding which detour namespaces to
# emit, so CMake has to honour it too. Globbing every feature .cpp regardless
# meant a disabled feature was still compiled while the symbols it references
# were deliberately not generated - which surfaces as a wall of
# "detour_X has not been declared" errors on a clean build, and only on a
# clean build, because an incremental one still has the old generated header.
# The old workaround was to rename the .cpp by hand when disabling a feature;
# that is no longer needed.
set(FEATURES_YAML "${CMAKE_CURRENT_SOURCE_DIR}/src/features/features.yaml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FEATURES_YAML}")

execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_hooks.py" --list-enabled
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE ENABLED_FEATURES_RAW
    RESULT_VARIABLE ENABLED_FEATURES_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT ENABLED_FEATURES_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not read enabled features from ${FEATURES_YAML}")
endif()
string(REPLACE "\n" ";" ENABLED_FEATURES "${ENABLED_FEATURES_RAW}")

set(FEATURE_SOURCES "")
set(FEATURE_DEFINES "")
foreach(feature IN LISTS ENABLED_FEATURES)
    string(STRIP "${feature}" feature)
    if(feature STREQUAL "")
        continue()
    endif()
    file(GLOB_RECURSE _feature_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/features/${feature}/*.cpp"
    )
    list(APPEND FEATURE_SOURCES ${_feature_sources})
    # SOF_FEATURE_<NAME> lets the shim's detach path call only the teardown
    # entry points that were actually compiled (see src/gamex86.cpp).
    string(TOUPPER "${feature}" _feature_upper)
    list(APPEND FEATURE_DEFINES "SOF_FEATURE_${_feature_upper}")
endforeach()
message(STATUS "Enabled features: ${ENABLED_FEATURES}")

set(SHIM_SOURCES
    src/gamex86.cpp
    src/log.cpp
    src/detours.cpp
    src/buddy_import.cpp
    src/shared_hook_manager.cpp
    third_party/DetourXS/ADE32.cpp
    third_party/DetourXS/detourxs.cpp
)

add_library(gamex86 SHARED
    ${SHIM_SOURCES}
    ${FEATURE_SOURCES}
    ${GENERATED_SOURCES}
    cmake/gamex86.def
)

# Remove the 'lib' prefix so it builds as 'gamex86.dll' instead of 'libgamex86.dll'
set_target_properties(gamex86 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gamex86"
)

target_compile_definitions(gamex86 PRIVATE ${FEATURE_DEFINES})

target_include_directories(gamex86 PRIVATE
    include
    third_party
    ${GENERATED_DIR}
)

target_link_libraries(gamex86 PRIVATE
    kernel32
    user32
    gdi32
    winspool
    shell32
    ole32
    oleaut32
    uuid
    comdlg32
    advapi32
)

target_link_options(gamex86 PRIVATE
    -static-libgcc
    -static-libstdc++
    -static
    -Wl,--stack,0x800000
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # -ggdb3: Maximum debug information including macro definitions
    # -gdwarf-4: Maximum compatibility with WineDbg and older GDB versions
    target_compile_options(gamex86 PRIVATE -ggdb3 -gdwarf-4)
endif()
