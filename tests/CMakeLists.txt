# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# No test declares a timeout. A test that hangs is a defect to diagnose, not
# something to kill, so CTest is left without a TIMEOUT property anywhere.

set(QOSFABRIC_TEST_TARGETS
  test_core
  test_engine
  test_registry
  test_store
  test_wire
  test_property
  test_concurrency)

foreach(test_name IN LISTS QOSFABRIC_TEST_TARGETS)
  add_executable(${test_name} ${test_name}.cpp harness.cpp)
  target_link_libraries(${test_name} PRIVATE qosfabric::qosfabric)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  qosfabric_apply_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The multi-process suite drives real operating-system processes over real
# framed loopback transport, so it is told where the built tools live.
add_executable(test_multiprocess test_multiprocess.cpp harness.cpp)
target_link_libraries(test_multiprocess PRIVATE qosfabric::qosfabric)
target_include_directories(test_multiprocess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
qosfabric_apply_warnings(test_multiprocess)
target_compile_definitions(test_multiprocess PRIVATE
  QOSFABRIC_CLI_PATH="$<TARGET_FILE:qosfabric_cli>"
  QOSFABRIC_PUBLISHER_PATH="$<TARGET_FILE:qosfabric_publisher>")
add_dependencies(test_multiprocess qosfabric_cli qosfabric_publisher)
add_test(NAME test_multiprocess COMMAND test_multiprocess)
