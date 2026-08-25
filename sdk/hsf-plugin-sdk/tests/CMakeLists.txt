# The SDK's own tests. These test the SDK, not any plugin: the mock transport,
# the blocking helper built on the non-blocking ABI, and the ABI contract
# itself.
hsf_add_plugin_test(NAME sdk_test_mock_transport SOURCES test_mock_transport.cpp)
hsf_add_plugin_test(NAME sdk_test_abi            SOURCES test_abi.cpp)

# The loader test dlopens the example plugin, so it needs the example built and
# needs to know where it landed. This is the test that proves the ABI actually
# works across a shared-object boundary rather than merely compiling.
if(HSF_SDK_BUILD_EXAMPLES)
  hsf_add_plugin_test(NAME sdk_test_loader SOURCES test_loader.cpp)
  add_dependencies(sdk_test_loader hsf_example_hello)
  target_compile_definitions(sdk_test_loader PRIVATE
    HSF_TEST_PLUGIN_PATH="$<TARGET_FILE:hsf_example_hello>")
endif()

# --- gateway-side plugin host tests ------------------------------------------
#
# These exercise src/plugin_manager -- gateway code, not SDK code -- but live
# here because this is where the C++ test harness is; the gateway has none of
# its own. Everything below is guarded so the SDK stays independently buildable
# when extracted from this repo, and so a missing dependency skips a test rather
# than breaking the build.
set(HSF_GATEWAY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(HSF_PM_DIR "${HSF_GATEWAY_ROOT}/src/plugin_manager")

if(NOT EXISTS "${HSF_PM_DIR}/PluginManifest.cpp")
  return()
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS
    "nlohmann_json not found; skipping the gateway-side plugin host tests. "
    "The SDK's own tests still build.")
  return()
endif()

hsf_add_plugin_test(
  NAME    sdk_test_manifest
  SOURCES test_manifest.cpp "${HSF_PM_DIR}/PluginManifest.cpp"
  INCLUDE_DIRS "${HSF_GATEWAY_ROOT}/include")
target_link_libraries(sdk_test_manifest PRIVATE nlohmann_json::nlohmann_json)

# The manager test drives the REAL example plugin through dlopen, so it needs
# the example built. It also pulls in Logger/LogStore/SqlDatabase, because the
# host services bridge to them -- hence SQLite.
find_package(SQLite3 QUIET)
if(HSF_SDK_BUILD_EXAMPLES AND SQLite3_FOUND)
  hsf_add_plugin_test(
    NAME    sdk_test_plugin_manager
    SOURCES test_plugin_manager.cpp
            "${HSF_PM_DIR}/PluginManifest.cpp"
            "${HSF_PM_DIR}/PluginLibrary.cpp"
            "${HSF_PM_DIR}/PluginHostServices.cpp"
            "${HSF_PM_DIR}/PluginManager.cpp"
            "${HSF_PM_DIR}/PluginTransports.cpp"
            "${HSF_GATEWAY_ROOT}/src/TcpSocket.cpp"
            "${HSF_GATEWAY_ROOT}/src/Logger.cpp"
            "${HSF_GATEWAY_ROOT}/src/LogStore.cpp"
            "${HSF_GATEWAY_ROOT}/src/SqlDatabase.cpp"
    INCLUDE_DIRS "${HSF_GATEWAY_ROOT}/include")
  target_link_libraries(sdk_test_plugin_manager
    PRIVATE nlohmann_json::nlohmann_json SQLite3::SQLite3)
  # PluginTransports reuses the gateway TcpSocket implementation. On Windows
  # that implementation imports Winsock directly, just like hsf_gateway.
  if(WIN32)
    target_link_libraries(sdk_test_plugin_manager PRIVATE ws2_32)
  endif()
  add_dependencies(sdk_test_plugin_manager hsf_example_hello)
  target_compile_definitions(sdk_test_plugin_manager PRIVATE
    HSF_TEST_PLUGIN_PATH="$<TARGET_FILE:hsf_example_hello>")
else()
  message(STATUS "SQLite3 not found; skipping sdk_test_plugin_manager.")
endif()
