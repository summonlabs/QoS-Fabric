# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

cmake_minimum_required(VERSION 3.20)

project(QoSFabric
  VERSION 1.0.0
  DESCRIPTION "Open-source, vendor-neutral C++20 runtime for authoritative end-to-end network service classes, obligations, compatibility, generations, and policy-bound QoS contracts across distributed fabric paths."
  HOMEPAGE_URL "https://github.com/summonlabs/QoS-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(QOSFABRIC_IS_TOPLEVEL ON)
else()
  set(QOSFABRIC_IS_TOPLEVEL OFF)
endif()

option(QOSFABRIC_BUILD_TESTS "Build the QoS Fabric test suites" ${QOSFABRIC_IS_TOPLEVEL})
option(QOSFABRIC_BUILD_TOOLS "Build the QoS Fabric command line tools" ${QOSFABRIC_IS_TOPLEVEL})
option(QOSFABRIC_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(QOSFABRIC_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(QOSFABRIC_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer where the toolchain supports it" OFF)
option(QOSFABRIC_ENABLE_STATIC_ANALYSIS "Enable the MSVC /analyze static analysis pass" OFF)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(QOSFABRIC_SOURCES
  src/version.cpp
  src/ids.cpp
  src/crypto.cpp
  src/serialize.cpp
  src/model.cpp
  src/decision.cpp
  src/store.cpp
  src/registry.cpp
  src/engine.cpp
  src/wire.cpp
  src/net.cpp)

set(QOSFABRIC_PUBLIC_HEADERS
  include/qosfabric/checked.hpp
  include/qosfabric/crypto.hpp
  include/qosfabric/decision.hpp
  include/qosfabric/engine.hpp
  include/qosfabric/error.hpp
  include/qosfabric/ids.hpp
  include/qosfabric/limits.hpp
  include/qosfabric/model.hpp
  include/qosfabric/net.hpp
  include/qosfabric/qosfabric.hpp
  include/qosfabric/registry.hpp
  include/qosfabric/serialize.hpp
  include/qosfabric/store.hpp
  include/qosfabric/version.hpp
  include/qosfabric/wire.hpp)

add_library(qosfabric ${QOSFABRIC_SOURCES} ${QOSFABRIC_PUBLIC_HEADERS})
add_library(qosfabric::qosfabric ALIAS qosfabric)

target_compile_features(qosfabric PUBLIC cxx_std_20)
set_target_properties(qosfabric PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(qosfabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

if(WIN32)
  target_link_libraries(qosfabric PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
function(qosfabric_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)
    if(QOSFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(QOSFABRIC_ENABLE_STATIC_ANALYSIS)
      target_compile_options(${target} PRIVATE /analyze /analyze:external-)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(QOSFABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(QOSFABRIC_ENABLE_ASAN)
    if(MSVC)
      # Debug information is required for actionable ASan reports; without it
      # the compiler emits C5072 and the report carries no source locations.
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /DEBUG /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(QOSFABRIC_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()
endfunction()

qosfabric_apply_warnings(qosfabric)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(QOSFABRIC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(QOSFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS qosfabric
  EXPORT QoSFabricTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT QoSFabricTargets
  FILE QoSFabricTargets.cmake
  NAMESPACE qosfabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QoSFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/QoSFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/QoSFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QoSFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/QoSFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/QoSFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/QoSFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/QoSFabric)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/QoSFabric)
