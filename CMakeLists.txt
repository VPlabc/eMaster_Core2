cmake_minimum_required(VERSION 3.20)

# Single source of truth for the version -- VERSION at the repo root, so the
# release tooling, the binary and the Git tag can never drift apart
# (request/release.md sections 6 and 27).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" HSF_VERSION LIMIT_COUNT 1)
string(STRIP "${HSF_VERSION}" HSF_VERSION)
if(NOT HSF_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
  message(FATAL_ERROR "VERSION must contain a semantic version like 1.0.0, got '${HSF_VERSION}'")
endif()

string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" HSF_PROJECT_VERSION "${HSF_VERSION}")
project(hsf_smart_gateway VERSION ${HSF_PROJECT_VERSION} LANGUAGES CXX)

# --- ZKTeco controller support -------------------------------------------
#
# src/zk_controller/ links plcommpro.dll in-process
# (request/HSF_Machine_ZK_Controller_Lua_Integration.md section 12). That DLL
# is a 32-bit Windows-only binary shipped by ZKTeco -- nothing here can
# rebuild or port it -- so enabling this forces the WHOLE gateway to x86.
#
# Defaulting it ON only where it can actually work keeps Linux, macOS and
# 64-bit Windows buildable (request/release.md section 1 lists all three as
# release targets). With it OFF, a stub PullSdkClient is compiled instead and
# every zk.* call fails with NotSupported rather than vanishing from the Lua
# API -- see src/zk_controller/PullSdkClient_stub.cpp.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(HSF_ENABLE_ZK_DEFAULT ON)
else()
  set(HSF_ENABLE_ZK_DEFAULT OFF)
endif()
option(HSF_ENABLE_ZK "Build ZKTeco PullSDK support (requires 32-bit Windows)" ${HSF_ENABLE_ZK_DEFAULT})

if(HSF_ENABLE_ZK)
  # Mixing an x64 hsf_gateway with the x86 plcommpro.dll is a linker error
  # waiting to happen -- fail fast with a clear message instead, the same
  # guard ZKTecoProtocol/fastread_sdk use for the same reason.
  if(NOT WIN32)
    message(FATAL_ERROR
      "HSF_ENABLE_ZK is ON but this is not a Windows build. The ZKTeco PullSDK "
      "(plcommpro.dll) is Windows-only. Re-run cmake with -DHSF_ENABLE_ZK=OFF.")
  endif()
  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "Win32" AND NOT CMAKE_VS_PLATFORM_NAME STREQUAL "Win32")
      message(FATAL_ERROR
        "HSF_ENABLE_ZK requires a Win32 (x86) build -- src/zk_controller links plcommpro.dll, "
        "a 32-bit-only DLL. Re-run cmake with -A Win32, or with -DHSF_ENABLE_ZK=OFF.")
    endif()
  elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
      "HSF_ENABLE_ZK requires a 32-bit (x86) compiler -- src/zk_controller links "
      "plcommpro.dll, a 32-bit-only DLL, and CMAKE_SIZEOF_VOID_P is ${CMAKE_SIZEOF_VOID_P}, not 4. "
      "Point CMAKE_C_COMPILER/CMAKE_CXX_COMPILER at the x86 (Hostx64\\x86) cl.exe and use the "
      "x86-windows vcpkg triplet, or re-run with -DHSF_ENABLE_ZK=OFF.")
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(SQLite3 REQUIRED)

# --- password hashing (request/AdvanceUpdate.md section 1.1) ---------------
#
# REQUIRED, unlike amqpcpp / the ZK PullSDK / OpenSSL above, all of which fall
# back to a stub. Those are optional features; authentication is not. With
# auth.enabled defaulting on, a gateway that cannot hash a password is a
# gateway whose web UI nobody can log into -- so a missing libsodium has to be
# a configure error here, not a surprise at the first login attempt.
find_package(unofficial-sodium CONFIG REQUIRED)

# --- RabbitMQ / AMQP support ----------------------------------------------
#
# AMQP-CPP is protocol-only: it ships no I/O of its own, which is exactly why it
# fits here -- src/MqClient.cpp drives it over the gateway's existing TcpSocket
# on its own thread, the same shape qt-mq-lab's MqConnection has over
# QTcpSocket (see that repo's RabbitMQ.md).
#
# QUIET, not REQUIRED: a triplet without the amqpcpp port must still build. With
# it missing, MqClient_stub.cpp is compiled instead and every Mq.* call fails
# with a clear message rather than the API disappearing -- the same treatment
# the ZK PullSDK gets above.
option(HSF_ENABLE_MQ "Build RabbitMQ (AMQP 0-9-1) support via AMQP-CPP" ON)

set(HSF_HAVE_MQ OFF)
if(HSF_ENABLE_MQ)
  find_package(amqpcpp CONFIG QUIET)
  if(amqpcpp_FOUND)
    set(HSF_HAVE_MQ ON)
  else()
    message(WARNING
      "HSF_ENABLE_MQ is ON but the amqpcpp package was not found. Building the "
      "RabbitMQ stub instead -- Mq.* will report that support is not compiled in. "
      "Add \"amqpcpp\" to vcpkg.json (it is there by default) and reconfigure, or "
      "pass -DHSF_ENABLE_MQ=OFF to silence this.")
  endif()
endif()

# --- plcommpro.lib generation from the hand-written .def (same recipe as
# ZKTecoProtocol/CMakeLists.txt "Design decision D4" and fastread_sdk) ---
if(HSF_ENABLE_ZK)
  get_filename_component(MSVC_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
  find_program(LIB_EXE NAMES lib.exe PATHS ${MSVC_BIN_DIR} NO_DEFAULT_PATH)
  if(NOT LIB_EXE)
    message(FATAL_ERROR "lib.exe not found next to ${CMAKE_CXX_COMPILER} -- is the C++ workload installed?")
  endif()

  set(PLCOMMPRO_DEF ${CMAKE_SOURCE_DIR}/third_party/plcommpro/plcommpro.def)
  set(PLCOMMPRO_LIB ${CMAKE_BINARY_DIR}/plcommpro.lib)
  add_custom_command(
    OUTPUT ${PLCOMMPRO_LIB}
    COMMAND ${LIB_EXE} /def:${PLCOMMPRO_DEF} /out:${PLCOMMPRO_LIB} /machine:x86
    DEPENDS ${PLCOMMPRO_DEF}
    COMMENT "Generating plcommpro.lib import library from plcommpro.def")
  add_custom_target(plcommpro_lib DEPENDS ${PLCOMMPRO_LIB})
  add_library(plcommpro STATIC IMPORTED)
  set_target_properties(plcommpro PROPERTIES IMPORTED_LOCATION ${PLCOMMPRO_LIB})
  add_dependencies(plcommpro plcommpro_lib)
endif()

set(HSF_SOURCES
  src/main.cpp
  src/Logger.cpp
  src/LogStore.cpp
  # General-purpose SQLite handle behind the Lua Db.* API and the locker UI's
  # read-only queries -- distinct from the three purpose-built stores
  # (config.db, logs.db, clients.db), which own their own schemas.
  src/SqlDatabase.cpp
  src/ConfigManager.cpp
  src/DynamicConfigManager.cpp
  src/LuaConfigSchemaLoader.cpp
  src/CardCache.cpp
  src/CardClientManager.cpp
  src/SystemMonitor.cpp
  src/RestClient.cpp
  src/SerialPort.cpp
  src/ModbusClient.cpp
  src/ModbusCodec.cpp
  src/ModbusProtocol.cpp
  src/ModbusRegisterStore.cpp
  src/ModbusRtu.cpp
  src/ModbusTcpSlave.cpp
  src/ModbusRegistry.cpp
  src/TcpSocket.cpp
  src/RfidClient.cpp
  # Envelope codec: no AMQP dependency, so it is compiled in both variants
  # below (a script can format a v1 payload even where it cannot publish one).
  src/MqCodec.cpp
  src/NetPing.cpp
  src/RuntimeVariables.cpp
  src/CitizenIdParser.cpp
  src/LuaEngine.cpp
  src/LuaRuntimeManager.cpp
  src/WebServer.cpp
  src/zk_controller/RTLogParser.cpp
  src/zk_controller/ZkController.cpp
  # C3/InBio panel protocol over plain TCP (request/deloyToUbuntu.md's added
  # scope; ported from qt-app-base's src/features/c3protocol). Compiled on
  # EVERY platform, unlike PullSdkClient: the whole point is that Linux and
  # 64-bit Windows -- where the 32-bit PullSDK DLL cannot load -- still get
  # real door control and card events.
  src/zk_controller/C3Codec.cpp
  src/zk_controller/C3Client.cpp
  src/plugin_manager/PluginTransports.cpp
  src/plugin_manager/PluginManifest.cpp
  src/plugin_manager/PluginLibrary.cpp
  src/plugin_manager/PluginHostServices.cpp
  src/plugin_manager/PluginManager.cpp
  # Over-the-air updates (request/CICD.md). Sha256 is unconditional: a package
  # whose bytes don't match the manifest has to be rejected on every build,
  # including one configured without OpenSSL below.
  # API security layer (request/AdvanceUpdate.md Phase 1): authentication,
  # RBAC, brute-force lockout, rate limiting and input validation.
  src/security/Permissions.cpp
  src/security/PasswordHash.cpp
  src/security/SecurityStore.cpp
  src/security/RateLimiter.cpp
  src/security/Validation.cpp
  # Production Lua packaging (request/AdvanceUpdate.md Phase 2): compile to
  # bytecode with the embedded interpreter, then encrypt and sign the result.
  src/lua_package/LuaBundle.cpp
  src/lua_package/LuaCompiler.cpp
  src/lua_package/LuaPackage.cpp
  src/lua_package/PackageKeys.cpp
  src/lua_package/PackageManager.cpp
  src/update/Version.cpp
  src/update/Sha256.cpp
  src/update/Downloader.cpp
  src/update/UpdateInstaller.cpp
  src/update/UpdateManager.cpp
  src/ffi/c3.cpp
  src/ffi/connectivity.cpp
)

# The only file that touches the PullSDK C ABI. Everything above it --
# ZkController, the Lua zk.* bindings, the dashboard card -- compiles
# unchanged either way; the stub just makes every call fail honestly.
if(HSF_ENABLE_ZK)
  list(APPEND HSF_SOURCES src/zk_controller/PullSdkClient.cpp)
else()
  list(APPEND HSF_SOURCES src/zk_controller/PullSdkClient_stub.cpp)
endif()

if(HSF_HAVE_MQ)
  list(APPEND HSF_SOURCES src/MqClient.cpp)
else()
  list(APPEND HSF_SOURCES src/MqClient_stub.cpp)
endif()

# --- update signature verification ----------------------------------------
#
# QUIET rather than REQUIRED, matching amqpcpp and the ZK PullSDK above: a
# triplet without an openssl port must still build. What differs is the shape
# of the stub. MqClient_stub makes Mq.* fail politely; SignatureVerifier_stub
# fails CLOSED -- every signature is rejected, so a build without OpenSSL can
# only take an over-the-air update if someone explicitly turns
# update.require_signature off. Treating "cannot verify" as "verified" would
# turn a missing dependency into remote code execution on a device that
# downloads its own updates.
#
# curl already links OpenSSL on Linux and macOS, so this is normally found
# without adding anything to the build; on Windows curl uses Schannel, which
# is why openssl is listed in vcpkg.json explicitly.
find_package(OpenSSL QUIET COMPONENTS Crypto)
if(OpenSSL_FOUND)
  set(HSF_HAVE_CRYPTO ON)
  list(APPEND HSF_SOURCES src/update/SignatureVerifier.cpp)
else()
  set(HSF_HAVE_CRYPTO OFF)
  list(APPEND HSF_SOURCES src/update/SignatureVerifier_stub.cpp)
  message(WARNING
    "OpenSSL was not found. Update packages cannot be signature-verified in this build; "
    "over-the-air updates will refuse to install unless update.require_signature is turned off. "
    "Add \"openssl\" to vcpkg.json (it is there by default) and reconfigure.")
endif()

if(WIN32)
  list(APPEND HSF_SOURCES src/SerialPort_win.cpp)
else()
  list(APPEND HSF_SOURCES src/SerialPort_posix.cpp)
endif()

add_executable(hsf_gateway ${HSF_SOURCES})

target_include_directories(hsf_gateway PRIVATE
  include
  third_party/plcommpro
  ${LUA_INCLUDE_DIR}
  # The plugin ABI headers. Referenced as a path rather than by linking
  # HSF::PluginSDK, for two reasons: the SDK subproject is added at the bottom
  # of this file, after this target exists, and the release builds turn it off
  # entirely (HSF_BUILD_PLUGIN_SDK=OFF) while src/plugin_manager still needs the
  # headers. Header-only, so there is nothing to link.
  sdk/hsf-plugin-sdk/include
)

target_link_libraries(hsf_gateway PRIVATE
  Threads::Threads
  CURL::libcurl
  nlohmann_json::nlohmann_json
  Crow::Crow
  ${LUA_LIBRARIES}
  SQLite3::SQLite3
  unofficial-sodium::sodium
  # dlopen/dlsym for the plugin loader. Empty on Windows (LoadLibrary is in
  # kernel32) and on glibc new enough to have folded libdl into libc, so this
  # expands to nothing there rather than failing to find a library.
  ${CMAKE_DL_LIBS}
)

if(HSF_HAVE_MQ)
  target_link_libraries(hsf_gateway PRIVATE amqpcpp)
  target_compile_definitions(hsf_gateway PRIVATE HSF_ENABLE_MQ=1)
endif()

if(HSF_HAVE_CRYPTO)
  target_link_libraries(hsf_gateway PRIVATE OpenSSL::Crypto)
  target_compile_definitions(hsf_gateway PRIVATE HSF_HAVE_CRYPTO=1)
endif()

if(WIN32)
  target_link_libraries(hsf_gateway PRIVATE ws2_32 mswsock iphlpapi)
  target_compile_definitions(hsf_gateway PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(HSF_ENABLE_ZK)
  target_link_libraries(hsf_gateway PRIVATE plcommpro)
  target_compile_definitions(hsf_gateway PRIVATE HSF_ENABLE_ZK=1)

  # plcommpro.dll (+ the 5 comm DLLs it loads) next to the built exe -- no
  # hard-coded dev-machine path (request/HSF_Machine_ZK_Controller_Lua_Integration.md
  # section 19), just relative to this project's own third_party/ directory.
  set(PLCOMMPRO_DLLS
    plcommpro.dll plcomms.dll plrscagent.dll plrscomm.dll pltcpcomm.dll plusbcomm.dll)
  foreach(dll ${PLCOMMPRO_DLLS})
    add_custom_command(TARGET hsf_gateway POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/third_party/plcommpro/bin/${dll}
        $<TARGET_FILE_DIR:hsf_gateway>)
  endforeach()
endif()

# --- Build metadata (request/release.md section 27) -----------------------
#
# Resolved at CONFIGURE time, not build time. The commit is therefore the one
# the tree was configured at; scripts/package.* re-runs cmake from a clean
# build directory so a release package always carries the right hash. A dirty
# working tree is marked, because "1.0.0 (a1b2c3d)" claiming to be a tagged
# build when it has uncommitted edits is worse than no hash at all.
find_package(Git QUIET)
set(HSF_GIT_COMMIT "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE HSF_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE HSF_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT HSF_GIT_DIRTY STREQUAL "")
    set(HSF_GIT_COMMIT "${HSF_GIT_COMMIT}-dirty")
  endif()
endif()

string(TIMESTAMP HSF_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)

# Normalised platform name, matching the package filenames in
# scripts/package.* and release.md section 16.
if(WIN32)
  set(HSF_PLATFORM_OS "windows")
elseif(APPLE)
  set(HSF_PLATFORM_OS "macos")
else()
  set(HSF_PLATFORM_OS "linux")
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" HSF_PLATFORM_ARCH)
if(HSF_PLATFORM_ARCH MATCHES "^(x86_64|amd64)$")
  set(HSF_PLATFORM_ARCH "x64")
elseif(HSF_PLATFORM_ARCH MATCHES "^(i.86|x86)$")
  set(HSF_PLATFORM_ARCH "x86")
elseif(HSF_PLATFORM_ARCH MATCHES "^(aarch64|arm64)$")
  set(HSF_PLATFORM_ARCH "arm64")
elseif(HSF_PLATFORM_ARCH MATCHES "^(arm|armv7l|armv8l|armhf)$")
  set(HSF_PLATFORM_ARCH "armhf")
endif()
# MSVC reports the HOST processor, so a Win32 build on an x64 machine would
# otherwise be labelled x64 -- take the pointer width as the authority.
if(WIN32 AND (CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64"))
  set(HSF_PLATFORM_ARCH "arm64")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(HSF_PLATFORM_ARCH "x86")
endif()
set(HSF_PLATFORM "${HSF_PLATFORM_OS}-${HSF_PLATFORM_ARCH}")

message(STATUS "eMaster Gateway ${HSF_VERSION} (${HSF_GIT_COMMIT}) for ${HSF_PLATFORM}, ZK=${HSF_ENABLE_ZK}, MQ=${HSF_HAVE_MQ}, crypto=${HSF_HAVE_CRYPTO}")

target_compile_definitions(hsf_gateway PRIVATE
  # Development fallbacks ONLY. At runtime the gateway first looks for web/
  # and config/ next to the executable (and in ../share/hsf_gateway/), so an
  # extracted release package is self-contained; these source-tree paths are
  # what makes running straight out of build-win/ work during development.
  # See ResolveResourceDir() in src/WebServer.cpp.
  HSF_WEB_ROOT="${CMAKE_SOURCE_DIR}/web"
  HSF_DEFAULT_CONFIG_PATH="${CMAKE_SOURCE_DIR}/config/config.db"

  HSF_VERSION="${HSF_VERSION}"
  HSF_GIT_COMMIT="${HSF_GIT_COMMIT}"
  HSF_BUILD_DATE="${HSF_BUILD_DATE}"
  HSF_PLATFORM="${HSF_PLATFORM}"
  HSF_COMPILER_ID="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)

install(TARGETS hsf_gateway RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION share/hsf_gateway/web)
install(DIRECTORY config/ DESTINATION share/hsf_gateway/config)
if(HSF_ENABLE_ZK)
  install(DIRECTORY third_party/plcommpro/bin/ DESTINATION bin)
endif()

# --- Plugin SDK ---------------------------------------------------------------
#
# Built as part of the gateway so its tests run in CI and a change to Core
# cannot quietly break the ABI plugins depend on. It contributes nothing to the
# hsf_gateway target: the SDK is header-only, and the gateway does not consume
# it yet (the Plugin Manager that will is a later phase). It is here purely so
# it is compiled and tested rather than rotting in a directory.
#
# On by default so an ordinary build compiles and tests it. The packaging
# scripts pass -DHSF_BUILD_PLUGIN_SDK=OFF, because the SDK's install() rules
# would otherwise put its headers into the release artifact, where
# scripts/package.sh's "no source files" check would rightly object to them.
# The SDK is distributed on its own, not inside the gateway package.
option(HSF_BUILD_PLUGIN_SDK "Build and test the plugin SDK alongside the gateway" ON)

if(HSF_BUILD_PLUGIN_SDK AND EXISTS "${CMAKE_SOURCE_DIR}/sdk/hsf-plugin-sdk/CMakeLists.txt")
  # enable_testing() must run at the top level so CTest writes the root
  # CTestTestfile.cmake. Calling it only inside the SDK/plugin subdirectories
  # makes their tests runnable by path, but `ctest --test-dir build` reports
  # "No tests were found" and never descends into them.
  enable_testing()
  add_executable(hsf_service_registry_tests tests/test_service_registry.cpp)
  target_include_directories(hsf_service_registry_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(hsf_service_registry_tests PRIVATE Threads::Threads)
  add_test(NAME hsf_service_registry_tests COMMAND hsf_service_registry_tests)
  set_tests_properties(hsf_service_registry_tests PROPERTIES TIMEOUT 120)
  add_executable(hsf_c3_ffi_tests
    tests/test_c3_ffi.cpp
    src/zk_controller/C3Codec.cpp
    src/zk_controller/C3Client.cpp
    src/TcpSocket.cpp
    src/Logger.cpp
    src/ffi/c3.cpp)
  target_include_directories(hsf_c3_ffi_tests PRIVATE include)
  target_link_libraries(hsf_c3_ffi_tests PRIVATE nlohmann_json::nlohmann_json)
  if(WIN32)
    target_link_libraries(hsf_c3_ffi_tests PRIVATE ws2_32)
  endif()
  add_test(NAME hsf_c3_ffi_tests COMMAND hsf_c3_ffi_tests)
  set_tests_properties(hsf_c3_ffi_tests PROPERTIES TIMEOUT 120)
  add_executable(hsf_connectivity_abi_tests
    tests/test_connectivity_abi.cpp
    src/ffi/connectivity.cpp)
  target_include_directories(hsf_connectivity_abi_tests PRIVATE include)
  add_test(NAME hsf_connectivity_abi_tests COMMAND hsf_connectivity_abi_tests)
  set_tests_properties(hsf_connectivity_abi_tests PROPERTIES TIMEOUT 120)
  add_executable(hsf_dynamic_config_tests
    tests/test_dynamic_config.cpp
    src/DynamicConfigManager.cpp)
  target_include_directories(hsf_dynamic_config_tests PRIVATE include)
  target_link_libraries(hsf_dynamic_config_tests PRIVATE nlohmann_json::nlohmann_json SQLite3::SQLite3)
  add_test(NAME hsf_dynamic_config_tests COMMAND hsf_dynamic_config_tests)
  set_tests_properties(hsf_dynamic_config_tests PROPERTIES TIMEOUT 120)
  add_executable(hsf_modbus_protocol_tests
    tests/test_modbus_protocol.cpp
    src/ModbusProtocol.cpp
    src/ModbusRegisterStore.cpp)
  target_include_directories(hsf_modbus_protocol_tests PRIVATE include)
  add_test(NAME hsf_modbus_protocol_tests COMMAND hsf_modbus_protocol_tests)
  set_tests_properties(hsf_modbus_protocol_tests PROPERTIES TIMEOUT 120)
  add_executable(hsf_modbus_rtu_tests
    tests/test_modbus_rtu.cpp
    src/ModbusRtu.cpp
    src/ModbusProtocol.cpp
    src/ModbusRegisterStore.cpp
    src/SerialPort.cpp
    src/Logger.cpp)
  if(WIN32)
    target_sources(hsf_modbus_rtu_tests PRIVATE src/SerialPort_win.cpp)
  else()
    target_sources(hsf_modbus_rtu_tests PRIVATE src/SerialPort_posix.cpp)
  endif()
  target_include_directories(hsf_modbus_rtu_tests PRIVATE include)
  target_link_libraries(hsf_modbus_rtu_tests PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME hsf_modbus_rtu_tests COMMAND hsf_modbus_rtu_tests)
  set_tests_properties(hsf_modbus_rtu_tests PROPERTIES TIMEOUT 120)
  # The subproject's own options decide what it builds; EXCLUDE_FROM_ALL would
  # skip its tests, which are the reason to build it here at all.
  add_subdirectory(sdk/hsf-plugin-sdk)
endif()

# P3 migrations are built with the gateway so their mock-transport regression
# tests run in the normal CTest invocation. They remain independently
# configurable plugin projects for third-party development.
if(HSF_BUILD_PLUGIN_SDK AND EXISTS "${CMAKE_SOURCE_DIR}/plugins/modbus/CMakeLists.txt")
  add_subdirectory(plugins/modbus)
endif()
if(HSF_BUILD_PLUGIN_SDK AND EXISTS "${CMAKE_SOURCE_DIR}/plugins/c3protocol/CMakeLists.txt")
  add_subdirectory(plugins/c3protocol)
endif()
