cmake_minimum_required(VERSION 3.25)

# C is enabled solely for FindHDF5: its module mode probes the HDF5 C library
# with a C test program (hit on Debian where no CMake config package exists).
project(NablaCore
  VERSION 0.1.0
  DESCRIPTION "Nabla AI physics-adaptive CFD solver core"
  HOMEPAGE_URL "https://nabla.ai"
  LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
# The core is a headless, UI-free, dependency-light C++20 library. It must only
# depend on permissively licensed code (BSD / MIT / Apache / Boost) — NEVER GPL.
# See ../CLAUDE.md for the architecture rules this build enforces.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build when the developer did not pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

option(NABLA_BUILD_TESTS "Build the Nabla core test suite (Catch2)" ON)
option(NABLA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Build provenance — short git SHA baked into the version header so every
# diagnostics file and report is traceable to an exact build (captured at
# configure time; re-run CMake after switching commits).
# Precedence: -DNABLA_GIT_SHA=… or env NABLA_GIT_SHA (container builds, where
# .git is not in the build context) > git rev-parse > "unknown".
# ---------------------------------------------------------------------------
if(NOT DEFINED NABLA_GIT_SHA OR NABLA_GIT_SHA STREQUAL "")
  set(NABLA_GIT_SHA "$ENV{NABLA_GIT_SHA}")
endif()
if(NABLA_GIT_SHA STREQUAL "")
  find_package(Git QUIET)
  if(Git_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
      OUTPUT_VARIABLE NABLA_GIT_SHA
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
endif()
if(NABLA_GIT_SHA STREQUAL "")
  set(NABLA_GIT_SHA "unknown")
endif()
message(STATUS "Nabla: version ${PROJECT_VERSION} (git ${NABLA_GIT_SHA})")

# ---------------------------------------------------------------------------
# Generated version header (single source of truth = project(VERSION ...))
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/nabla/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Warning flags helper
# ---------------------------------------------------------------------------
add_library(nabla_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(nabla_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(NABLA_WARNINGS_AS_ERRORS)
    target_compile_options(nabla_warnings INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(nabla_warnings INTERFACE /W4 /permissive-)
  if(NABLA_WARNINGS_AS_ERRORS)
    target_compile_options(nabla_warnings INTERFACE /WX)
  endif()
endif()

# ---------------------------------------------------------------------------
# nabla_core — static solver library
# ---------------------------------------------------------------------------
add_library(nabla_core STATIC
  src/field.cpp
  src/solver.cpp
  src/io.cpp
  src/mesh/octree.cpp
  src/io/vtu_writer.cpp
  src/io/checkpoint.cpp
  src/geometry/stl_reader.cpp
  src/geometry/solid_geometry.cpp
  src/geometry/features.cpp
  src/geometry/classify.cpp
  src/geometry/geometry_report.cpp
  src/geometry/ingest.cpp
  src/flow/poisson.cpp
  src/flow/multigrid.cpp
  src/flow/octree_poisson.cpp
  src/flow/ns_solver.cpp
  src/flow/case.cpp
  src/adaptive/adaptive_core.cpp
  src/adaptive/audit.cpp
  src/adaptive/transport.cpp
  src/adaptive/run.cpp)
add_library(nabla::core ALIAS nabla_core)

target_compile_features(nabla_core PUBLIC cxx_std_20)
target_include_directories(nabla_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>")
target_link_libraries(nabla_core PRIVATE nabla_warnings)

# ---------------------------------------------------------------------------
# HDF5 (optional) — permissive BSD-style license, OK for the core.
# Used for binary checkpoint round-trips. When absent, checkpoint I/O throws.
# ---------------------------------------------------------------------------
option(NABLA_WITH_HDF5 "Enable HDF5 checkpoint support" ON)
if(NABLA_WITH_HDF5)
  find_package(HDF5 COMPONENTS C QUIET)
  if(HDF5_FOUND)
    target_include_directories(nabla_core PUBLIC ${HDF5_INCLUDE_DIRS})
    target_link_libraries(nabla_core PUBLIC ${HDF5_LIBRARIES})
    target_compile_definitions(nabla_core PUBLIC NABLA_HAVE_HDF5)
    message(STATUS "Nabla: HDF5 checkpoint support enabled (${HDF5_VERSION})")
  else()
    message(STATUS "Nabla: HDF5 not found — checkpoint I/O will be unavailable")
  endif()
endif()

# ---------------------------------------------------------------------------
# nabla_solve — command-line front-end (file-based IPC entry point)
# ---------------------------------------------------------------------------
add_executable(nabla_solve apps/nabla_solve/main.cpp)
target_link_libraries(nabla_solve PRIVATE nabla_core nabla_warnings)

# ---------------------------------------------------------------------------
# nabla_mesh_demo — octree AMR demonstration (writes .vtu + checkpoint)
# ---------------------------------------------------------------------------
add_executable(nabla_mesh_demo apps/nabla_mesh_demo/main.cpp)
target_link_libraries(nabla_mesh_demo PRIVATE nabla_core nabla_warnings)

# ---------------------------------------------------------------------------
# nabla_tests — Catch2 unit tests
# ---------------------------------------------------------------------------
if(NABLA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
