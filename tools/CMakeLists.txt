# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(qosfabric_cli qosfabric_cli.cpp)
target_link_libraries(qosfabric_cli PRIVATE qosfabric::qosfabric)
qosfabric_apply_warnings(qosfabric_cli)

add_executable(qosfabric_publisher qosfabric_publisher.cpp)
target_link_libraries(qosfabric_publisher PRIVATE qosfabric::qosfabric)
qosfabric_apply_warnings(qosfabric_publisher)

add_executable(qosfabric_bench qosfabric_bench.cpp)
target_link_libraries(qosfabric_bench PRIVATE qosfabric::qosfabric)
qosfabric_apply_warnings(qosfabric_bench)

install(TARGETS qosfabric_cli qosfabric_publisher qosfabric_bench
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
