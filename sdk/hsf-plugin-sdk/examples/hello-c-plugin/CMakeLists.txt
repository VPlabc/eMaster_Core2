cmake_minimum_required(VERSION 3.16)
project(hsf_c_driver_example LANGUAGES C)

# When this example is configured as part of the SDK tree the interface target
# already exists.  find_package() is only needed when the example is consumed
# as a standalone project; the package config is generated at install time and
# therefore cannot be found during this in-tree configure.
if(NOT TARGET HSF::PluginSDKC)
  find_package(HSFPluginSDK REQUIRED)
endif()

hsf_add_c_plugin(
  NAME hsf.driver.c_example
  SOURCES src/c_example_driver.c
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json)

set_target_properties(hsf_driver_c_example PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
