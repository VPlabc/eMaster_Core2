# A standalone plugin project. Nothing here knows about the gateway's build:
# it needs only the SDK, which is the point (plan §26 — a plugin author must
# not have to install the whole gateway development environment).
#
#   cmake -B build -DCMAKE_PREFIX_PATH=/path/to/installed/sdk
#   cmake --build build
#   ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(hsf_driver_example VERSION 0.1.0 LANGUAGES CXX)

find_package(HSFPluginSDK REQUIRED)

hsf_add_plugin(
  NAME    hsf.driver.example
  SOURCES src/example_driver.cpp
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

# Unit tests link the driver's sources directly rather than loading the .so:
# they are testing the protocol logic, and going through dlopen would only
# make failures harder to read. The SDK's own loader test already covers the
# ABI, so there is nothing gained by re-proving it per plugin.
hsf_add_plugin_test(
  NAME    example_driver_tests
  SOURCES tests/test_driver.cpp src/example_driver.cpp
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(example_driver_tests PRIVATE HSF_PLUGIN_TESTING=1)
