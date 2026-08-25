cmake_minimum_required(VERSION 3.16)
project(hsf_driver_c3protocol VERSION 1.1.2 LANGUAGES C CXX)

if(NOT TARGET HSF::PluginSDK)
  find_package(HSFPluginSDK REQUIRED)
endif()
find_package(nlohmann_json CONFIG REQUIRED)

hsf_add_plugin(
  NAME hsf.driver.c3protocol
  SOURCES src/c3_driver_c.c)

enable_testing()
hsf_add_plugin_test(
  NAME c3protocol_driver_tests
  SOURCES tests/test_c3_driver.cpp src/c3_driver.cpp ${CMAKE_SOURCE_DIR}/src/zk_controller/C3Codec.cpp
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include
  LINK_LIBS nlohmann_json::nlohmann_json)
target_compile_definitions(c3protocol_driver_tests PRIVATE HSF_PLUGIN_TESTING=1)
