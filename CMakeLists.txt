cmake_minimum_required(VERSION 3.28)
project(syntwine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

find_package(Python 3.15 REQUIRED COMPONENTS Interpreter Development.Module)
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 3.1.0 CONFIG QUIET)
if(NOT pybind11_FOUND)
    # A pip installation puts its CMake package under the interpreter's site-packages.
    set(pybind11_cmake_dir "")
    if(NOT CMAKE_CROSSCOMPILING)
        execute_process(
            COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
            OUTPUT_VARIABLE pybind11_cmake_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    find_package(pybind11 3.1.0 CONFIG REQUIRED HINTS "${pybind11_cmake_dir}")
endif()
include(cmake/Dependencies.cmake)

add_library(syntwine_platform STATIC
    src/platform/tokenizer.cpp
    src/platform/writer.cpp)
target_include_directories(syntwine_platform PRIVATE include)
target_link_libraries(syntwine_platform PRIVATE pcre2-8)

# Compile large binding translation units with bounded peak memory even in wheel builders.
set(SYNTWINE_COMPILE_JOBS 4 CACHE STRING "Maximum concurrent binding compilations with Ninja")
set_property(GLOBAL PROPERTY JOB_POOLS syntwine_cxx=${SYNTWINE_COMPILE_JOBS})
pybind11_add_module(_syntwine_native MODULE NO_EXTRAS
    src/bindings/module.cpp
    src/bindings/core_bindings.cpp
    src/bindings/budget.cpp
    src/core/core.cpp
    src/core/document.cpp
    src/formats/text.cpp
    src/formats/text_algorithms.cpp
    src/formats/markdown.cpp
    src/formats/json.cpp
    src/formats/toml.cpp
    src/formats/xml.cpp
    src/telemetry/telemetry.cpp
    src/telemetry/telemetry_sinks.cpp)
target_include_directories(_syntwine_native PRIVATE include)
set_property(TARGET _syntwine_native PROPERTY JOB_POOL_COMPILE syntwine_cxx)
target_link_libraries(_syntwine_native PRIVATE syntwine_platform syntwine_sqlite
    LibXml2::LibXml2 cmark jsoncons tomlplusplus::tomlplusplus CURL::libcurl)
if(MSVC)
    target_compile_options(syntwine_platform PRIVATE /utf-8)
    target_compile_options(_syntwine_native PRIVATE /utf-8 /bigobj /Zc:__cplusplus)
    target_compile_definitions(_syntwine_native PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(_syntwine_native PRIVATE "LINKER:--exclude-libs,ALL")
endif()
install(TARGETS _syntwine_native LIBRARY DESTINATION syntwine)

option(SYNTWINE_NATIVE_TESTS "Build and run native regression tests" OFF)
if(SYNTWINE_NATIVE_TESTS)
    enable_testing()
    add_executable(syntwine_native_tests tests/native/test_platform.cpp)
    target_include_directories(syntwine_native_tests PRIVATE include)
    target_link_libraries(syntwine_native_tests PRIVATE syntwine_platform)
    if(MSVC)
        target_compile_options(syntwine_native_tests PRIVATE /utf-8)
    endif()
    add_test(NAME syntwine_native COMMAND syntwine_native_tests
        "${PROJECT_SOURCE_DIR}/python/syntwine/tokenizer" "${PROJECT_BINARY_DIR}")
    add_custom_target(syntwine_check ALL
        COMMAND "${CMAKE_CTEST_COMMAND}" --build-config "$<CONFIG>" --output-on-failure
        DEPENDS syntwine_native_tests
        USES_TERMINAL)
endif()
