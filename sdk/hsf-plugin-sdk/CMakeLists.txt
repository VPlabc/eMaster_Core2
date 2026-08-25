# HSF Plugin SDK.
#
# Versioned independently of the gateway (plan §6): this project's version is
# the Plugin API version, and it is deliberately not 1.0.0-the-gateway.
cmake_minimum_required(VERSION 3.16)
project(HSFPluginSDK VERSION 1.0.0 LANGUAGES C CXX)

# Keep the platform identity in the ABI metadata emitted by the examples and
# tests.  It must be a literal compile definition: HSF_STR_LIT() is deliberately
# usable in static initialisers and therefore cannot consume a runtime string.
if(WIN32)
  set(HSF_SDK_PLATFORM_OS "windows")
elseif(APPLE)
  set(HSF_SDK_PLATFORM_OS "macos")
else()
  set(HSF_SDK_PLATFORM_OS "linux")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(HSF_SDK_PLATFORM_ARCH "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64|arm64)$")
  set(HSF_SDK_PLATFORM_ARCH "arm64")
else()
  set(HSF_SDK_PLATFORM_ARCH "x64")
endif()
set(HSF_PLATFORM_TRIPLE "${HSF_SDK_PLATFORM_OS}-${HSF_SDK_PLATFORM_ARCH}")

option(HSF_SDK_BUILD_TESTS "Build the SDK's own tests" ON)
option(HSF_SDK_BUILD_EXAMPLES "Build the example plugins" ON)

# Header-only, so INTERFACE. A plugin links no SDK object code at all — which
# is what makes the ABI the only coupling between a plugin and the host, and
# means a plugin built against 1.0 headers keeps working when the host's own
# code changes underneath it.
add_library(hsf_plugin_sdk INTERFACE)
add_library(HSF::PluginSDK ALIAS hsf_plugin_sdk)

# C plugins use the same C ABI headers, but must not inherit the C++17
# interface feature from the historical C++ target.
add_library(hsf_plugin_sdk_c INTERFACE)
add_library(HSF::PluginSDKC ALIAS hsf_plugin_sdk_c)

target_compile_definitions(hsf_plugin_sdk INTERFACE HSF_PLATFORM_TRIPLE="${HSF_PLATFORM_TRIPLE}")
target_compile_definitions(hsf_plugin_sdk_c INTERFACE HSF_PLATFORM_TRIPLE="${HSF_PLATFORM_TRIPLE}")

# EXPORT_NAME, or an installed SDK is unusable.
#
# An ALIAS target exists only in THIS build tree; it is not exported. Without
# this, `install(EXPORT ... NAMESPACE HSF::)` publishes the target under its
# real name as HSF::hsf_plugin_sdk, while hsf_add_plugin() — and every plugin
# author following the documented example — links HSF::PluginSDK. The result is
# "target not found" for anyone consuming the SDK through find_package, while
# building inside this repo keeps working, because in here the alias is real.
# Setting the export name makes both spellings the same one.
set_target_properties(hsf_plugin_sdk PROPERTIES EXPORT_NAME PluginSDK)
set_target_properties(hsf_plugin_sdk_c PROPERTIES EXPORT_NAME PluginSDKC)

target_include_directories(hsf_plugin_sdk INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_include_directories(hsf_plugin_sdk_c INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_compile_features(hsf_plugin_sdk INTERFACE cxx_std_17)

# dlopen lives in libdl on older glibc. A plugin does not need it, but a HOST
# does, and the loader smoke test below is a host.
if(UNIX AND NOT APPLE)
  target_link_libraries(hsf_plugin_sdk INTERFACE ${CMAKE_DL_LIBS})
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/HSFPluginSDK.cmake)

if(HSF_SDK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HSF_SDK_BUILD_EXAMPLES)
  add_subdirectory(examples/hello-plugin)
  add_subdirectory(examples/hello-c-plugin)
endif()

# --- install / find_package ---------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS hsf_plugin_sdk hsf_plugin_sdk_c EXPORT HSFPluginSDKTargets)
install(FILES cmake/HSFPluginSDK.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HSFPluginSDK)
install(EXPORT HSFPluginSDKTargets
        FILE HSFPluginSDKTargets.cmake
        NAMESPACE HSF::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HSFPluginSDK)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/HSFPluginSDKConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  # SameMajorVersion mirrors the ABI rule in version.h: a major bump is
  # incompatible, a minor one is additive. The build-time check and the
  # load-time check must agree, or one of them is lying.
  COMPATIBILITY SameMajorVersion)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/HSFPluginSDKConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/HSFPluginSDKConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HSFPluginSDK)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/HSFPluginSDKConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/HSFPluginSDKConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HSFPluginSDK)
