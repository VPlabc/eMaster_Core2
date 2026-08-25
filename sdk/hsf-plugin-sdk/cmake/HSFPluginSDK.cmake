# HSF Plugin SDK — CMake helpers (plan §24).
#
# A plugin author writes:
#
#     find_package(HSFPluginSDK REQUIRED)
#     hsf_add_plugin(NAME hsf.driver.modbus SOURCES src/modbus.cpp)
#
# and gets include paths, ABI flags, the right output name, hidden symbol
# visibility, and a manifest check — without knowing any of the rules below.

include_guard(GLOBAL)

# The dotted plugin id is not a legal CMake target name, so every helper keeps
# the two apart: `hsf.driver.modbus` is the id, `hsf_driver_modbus` the target.
function(_hsf_target_name out id)
  string(REPLACE "." "_" safe "${id}")
  set(${out} "${safe}" PARENT_SCOPE)
endfunction()

# hsf_add_plugin(NAME <id> SOURCES <src>... [MANIFEST <path>]
#                [INCLUDE_DIRS <dir>...] [LINK_LIBS <lib>...] [NO_MANIFEST])
#
# Produces a MODULE library named plugin.so / plugin.dll — the fixed filename
# the manifest's "entry" field names, so the Plugin Manager does not have to
# guess or glob. The directory is what identifies the plugin, per plan §11:
#
#     plugins/modbus/plugin.so
#     plugins/modbus/manifest.json
function(hsf_add_plugin)
  cmake_parse_arguments(A "NO_MANIFEST" "NAME;MANIFEST;OUTPUT_DIR"
                        "SOURCES;INCLUDE_DIRS;LINK_LIBS" ${ARGN})

  if(NOT A_NAME)
    message(FATAL_ERROR "hsf_add_plugin: NAME is required (e.g. hsf.driver.modbus)")
  endif()
  if(NOT A_SOURCES)
    message(FATAL_ERROR "hsf_add_plugin(${A_NAME}): SOURCES is required")
  endif()

  _hsf_target_name(tgt "${A_NAME}")

  # MODULE, not SHARED: a plugin is dlopen'd, never linked against. Saying so
  # stops CMake from generating an import library on Windows that nothing uses.
  add_library(${tgt} MODULE ${A_SOURCES})

  target_link_libraries(${tgt} PRIVATE HSF::PluginSDK ${A_LINK_LIBS})
  if(A_INCLUDE_DIRS)
    target_include_directories(${tgt} PRIVATE ${A_INCLUDE_DIRS})
  endif()

  target_compile_features(${tgt} PRIVATE cxx_std_17)

  set_target_properties(${tgt} PROPERTIES
    OUTPUT_NAME "plugin"
    PREFIX ""                      # no "lib": the entry is exactly plugin.so
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

  if(A_OUTPUT_DIR)
    set_target_properties(${tgt} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${A_OUTPUT_DIR}")
  endif()

  # Hidden visibility is not cosmetic. Two plugins loaded into one process that
  # each carry a differently-built copy of some common helper will, with default
  # visibility, resolve to whichever loaded first — a genuine crash that only
  # appears once a second plugin is installed. Exporting only the four ABI
  # entry points makes that unrepresentable.
  if(NOT MSVC)
    target_link_options(${tgt} PRIVATE
      $<$<PLATFORM_ID:Linux>:LINKER:--no-undefined>)
  endif()

  # Stage the manifest beside the binary so the build output is directly
  # loadable, without a packaging step, during development.
  if(NOT A_NO_MANIFEST)
    set(manifest "${A_MANIFEST}")
    if(NOT manifest)
      set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")
    endif()
    if(EXISTS "${manifest}")
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${manifest}" "$<TARGET_FILE_DIR:${tgt}>/manifest.json"
        COMMENT "Staging manifest for ${A_NAME}")
    else()
      # A warning, not an error: a manifest-less build is useful while
      # bisecting a compile problem. The Plugin Manager will refuse to install
      # it, which is the right place for that to be fatal.
      message(WARNING
        "hsf_add_plugin(${A_NAME}): no manifest at ${manifest}. "
        "The plugin will build but cannot be installed. Pass NO_MANIFEST to silence.")
    endif()
  endif()
endfunction()

# hsf_add_c_plugin(NAME <id> SOURCES <src>... [MANIFEST <path>]
#                   [INCLUDE_DIRS <dir>...] [LINK_LIBS <lib>...]
#                   [NO_MANIFEST])
function(hsf_add_c_plugin)
  cmake_parse_arguments(A "NO_MANIFEST" "NAME;MANIFEST;OUTPUT_DIR"
                        "SOURCES;INCLUDE_DIRS;LINK_LIBS" ${ARGN})
  if(NOT A_NAME)
    message(FATAL_ERROR "hsf_add_c_plugin: NAME is required")
  endif()
  if(NOT A_SOURCES)
    message(FATAL_ERROR "hsf_add_c_plugin(${A_NAME}): SOURCES are required")
  endif()

  _hsf_target_name(tgt "${A_NAME}")
  add_library(${tgt} MODULE ${A_SOURCES})
  target_link_libraries(${tgt} PRIVATE HSF::PluginSDKC ${A_LINK_LIBS})
  if(A_INCLUDE_DIRS)
    target_include_directories(${tgt} PRIVATE ${A_INCLUDE_DIRS})
  endif()
  set_target_properties(${tgt} PROPERTIES
    OUTPUT_NAME "plugin"
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)
  if(A_OUTPUT_DIR)
    set_target_properties(${tgt} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${A_OUTPUT_DIR}")
  endif()
  if(NOT MSVC)
    target_link_options(${tgt} PRIVATE
      $<$<PLATFORM_ID:Linux>:LINKER:--no-undefined>)
  endif()

  if(NOT A_NO_MANIFEST)
    set(manifest "${A_MANIFEST}")
    if(NOT manifest)
      set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json")
    endif()
    if(EXISTS "${manifest}")
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${manifest}" "$<TARGET_FILE_DIR:${tgt}>/manifest.json"
        COMMENT "Staging manifest for ${A_NAME}")
    else()
      message(WARNING "hsf_add_c_plugin(${A_NAME}): no manifest at ${manifest}")
    endif()
  endif()
endfunction()

# hsf_add_plugin_test(NAME <target> SOURCES <src>... [LINK_LIBS <lib>...])
#
# A test executable with the SDK and its header-only harness available, wired
# into ctest. Deliberately not a separate framework: see hsf/testing.hpp for
# why the SDK ships its own.
function(hsf_add_plugin_test)
  cmake_parse_arguments(A "" "NAME" "SOURCES;LINK_LIBS;INCLUDE_DIRS" ${ARGN})
  if(NOT A_NAME OR NOT A_SOURCES)
    message(FATAL_ERROR "hsf_add_plugin_test: NAME and SOURCES are required")
  endif()

  add_executable(${A_NAME} ${A_SOURCES})
  target_link_libraries(${A_NAME} PRIVATE HSF::PluginSDK ${A_LINK_LIBS})
  if(A_INCLUDE_DIRS)
    target_include_directories(${A_NAME} PRIVATE ${A_INCLUDE_DIRS})
  endif()
  target_compile_features(${A_NAME} PRIVATE cxx_std_17)

  add_test(NAME ${A_NAME} COMMAND ${A_NAME})
  # A test that hangs must fail rather than occupy CI forever.
  set_tests_properties(${A_NAME} PROPERTIES TIMEOUT 120)
endfunction()
