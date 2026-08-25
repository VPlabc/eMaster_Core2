cmake_minimum_required(VERSION 3.16)
project(hsf_driver_modbus VERSION 1.1.0 LANGUAGES C CXX)

# Built in-tree for gateway regression coverage; this project can also be
# configured independently after installing HSFPluginSDK.
if(NOT TARGET HSF::PluginSDK)
  find_package(HSFPluginSDK REQUIRED)
endif()

hsf_add_plugin(
  NAME hsf.driver.modbus
  SOURCES src/modbus_driver_c.c)

enable_testing()
hsf_add_plugin_test(
  NAME modbus_driver_tests
  SOURCES tests/test_modbus_driver.cpp src/modbus_driver.cpp
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(modbus_driver_tests PRIVATE HSF_PLUGIN_TESTING=1)
