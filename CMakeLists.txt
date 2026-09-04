# SPDX-License-Identifier: LicenseRef-OBINexus-1.0
#
# MMUKO kernel dynamic C ABI.
#
#   cmake -B build/cmake -S . && cmake --build build/cmake && ctest --test-dir build/cmake
#
# The Makefile is the primary build and is what the ring-0 and demo harnesses
# use. This file exists so the library can be consumed by a CMake project --
# `find_package` / `add_subdirectory` and an imported target -- and so CI on a
# host without GNU make still has a way in.
#
# Only the 64-bit profile is built here. The dual-profile matrix, the QEMU boot
# test and the Rust suites are driven by the Makefile, because expressing a
# freestanding cross-profile build in CMake would cost more clarity than it buys.

cmake_minimum_required(VERSION 3.16)
project(mmuko_abi
        VERSION 0.1.0
        DESCRIPTION "MMUKO kernel dynamic C ABI: SemVerX trident consensus binding"
        LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# -Wconversion is on deliberately. This library's whole subject is the silent
# reinterpretation of values across a boundary; tolerating implicit narrowing
# inside it would be making a joke of itself.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion
                      -Wstrict-prototypes -Wmissing-prototypes
                      -Wpointer-arith -Wcast-qual -Wwrite-strings)
endif()

# ---------------------------------------------------------------------------
# mmuko_abi_core -- freestanding. No libc, no allocator, no dynamic loader.
# This is the target a kernel links.
# ---------------------------------------------------------------------------

add_library(mmuko_abi_core STATIC
  src/mmuko_abi.c
  src/mmuko_semverx.c
  src/mmuko_desc.c
  src/mmuko_trident.c)

target_include_directories(mmuko_abi_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

set_target_properties(mmuko_abi_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)

add_library(mmuko::abi_core ALIAS mmuko_abi_core)

# ---------------------------------------------------------------------------
# mmuko_abi -- core plus the hosted dynamic loader. Needs dlopen.
# ---------------------------------------------------------------------------

add_library(mmuko_abi STATIC
  src/mmuko_loader.c
  tools/mmuko_layout_probe.c)

target_link_libraries(mmuko_abi PUBLIC mmuko_abi_core)
set_target_properties(mmuko_abi PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(UNIX AND NOT APPLE)
  target_link_libraries(mmuko_abi PUBLIC ${CMAKE_DL_LIBS})
endif()

add_library(mmuko::abi ALIAS mmuko_abi)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

add_executable(mmuko-abi-dump tools/mmuko_abi_dump.c)
target_link_libraries(mmuko-abi-dump PRIVATE mmuko_abi)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(MMUKO_BUILD_TESTS "Build the MMUKO ABI test suites" ON)

if(MMUKO_BUILD_TESTS)
  enable_testing()

  # Fixture shared objects. Each carries its own copy of the encoder, exactly
  # as a real third-party module would -- which is also what makes the
  # differential meaningful: the fingerprints are computed by the module's own
  # code, not by the loader's.
  foreach(fixture mathlib_v1 mathlib_v2 mathlib_v1_1 mathlib_v2_stable)
    add_library(${fixture} SHARED
      tests/fixtures/${fixture}.c
      src/mmuko_abi.c
      src/mmuko_semverx.c)
    target_include_directories(${fixture} PRIVATE include)
    set_target_properties(${fixture} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
      C_VISIBILITY_PRESET hidden)
  endforeach()

  # The undeclared object from the transcript. No ABI table, and therefore
  # unloadable -- which is the behaviour under test.
  add_library(mathlib_bare SHARED tests/fixtures/mathlib_bare.c)
  set_target_properties(mathlib_bare PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  foreach(suite test_fingerprint test_semverx test_trident test_loader)
    add_executable(${suite} tests/${suite}.c)
    target_include_directories(${suite} PRIVATE include tests)
    target_link_libraries(${suite} PRIVATE mmuko_abi)
    add_test(NAME ${suite}
             COMMAND ${suite} ${CMAKE_BINARY_DIR}
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endforeach()

  add_dependencies(test_loader
    mathlib_v1 mathlib_v2 mathlib_v1_1 mathlib_v2_stable mathlib_bare)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS mmuko_abi_core mmuko_abi mmuko-abi-dump
        EXPORT mmuko-abi-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/mmuko DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT mmuko-abi-targets
        FILE mmuko-abi-targets.cmake
        NAMESPACE mmuko::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mmuko-abi)
