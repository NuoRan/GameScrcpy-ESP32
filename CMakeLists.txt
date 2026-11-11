cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/extension/manifest.json" gs_manifest)
string(JSON gs_extension_id GET "${gs_manifest}" id)
string(JSON gs_extension_version GET "${gs_manifest}" version)

project(GameScrcpyEsp32
    VERSION ${gs_extension_version}
    DESCRIPTION "The ESP32 touch box for GameScrcpy as a community extension"
    LANGUAGES C CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "The ESP32 touch box extension is for GameScrcpy on Windows 10/11 only.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

option(GS_BUILD_TESTS "Build the unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

set(GS_PACKAGE_DIR "${PROJECT_BINARY_DIR}/package")

include(GsCompilerPolicy)

if(GS_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(native)
add_subdirectory(extension)

if(GS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
