# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# An independent downstream consumer. It is deliberately *not* added by the QoS
# Fabric build: it resolves the installed package through find_package, exactly
# as an external project would.

cmake_minimum_required(VERSION 3.20)
project(QoSFabricConsumer LANGUAGES CXX)

find_package(QoSFabric 1.0 CONFIG REQUIRED)

add_executable(qosfabric_consumer main.cpp)
target_link_libraries(qosfabric_consumer PRIVATE qosfabric::qosfabric)
set_target_properties(qosfabric_consumer PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(qosfabric_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(qosfabric_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "QoSFabric found: ${QoSFabric_VERSION} (${QoSFabric_DIR})")
