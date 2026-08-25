cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

project(tNVSE LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "tNVSE requires the 32-bit Microsoft Visual C++ toolchain.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "tNVSE is an NVSE plugin and must be configured for Win32, not x64.")
endif()
if(CMAKE_VS_PLATFORM_TOOLSET AND NOT CMAKE_VS_PLATFORM_TOOLSET STREQUAL "v145")
  message(FATAL_ERROR "tNVSE requires PlatformToolset v145; selected: ${CMAKE_VS_PLATFORM_TOOLSET}")
endif()
if(MSVC_TOOLSET_VERSION AND MSVC_TOOLSET_VERSION VERSION_LESS 145)
  message(FATAL_ERROR "tNVSE requires MSVC 14.5x/v145; selected: ${MSVC_TOOLSET_VERSION}")
endif()

if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported tNVSE configurations" FORCE)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT tnvse)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TnvseMsvc)
include(TnvseDependencies)
include(TnvseShaders)

tnvse_add_dxsdk_target()
tnvse_add_vcltl_target()
tnvse_add_commonlib_target()
tnvse_add_freetype_target()
tnvse_add_msdfgen_target()
tnvse_add_shader_target(TNVSE_SHADER_OUTPUTS)

option(TNVSE_BUILD_FONT_QUALITY_PROBE
  "Build the offline FreeType/msdfgen quality comparison tool" OFF)
if(TNVSE_BUILD_FONT_QUALITY_PROBE)
  add_executable(tnvse_font_quality_probe
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/font_distance_field_quality_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font/vector/font_vector_msdfgen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font/vector/font_vector_msdfgen_common.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font/vector/font_vector_msdfgen_mtsdf.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font/vector/font_vector_msdfgen_mtsdf_rescue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font/vector/font_vector_msdfgen_true_sdf.cpp")
  target_compile_features(tnvse_font_quality_probe PRIVATE cxx_std_20)
  target_include_directories(tnvse_font_quality_probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src")
  target_compile_definitions(tnvse_font_quality_probe PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(tnvse_font_quality_probe PRIVATE
    /utf-8 /wd4244 /wd4267 /wd4996
    "$<$<CONFIG:Release>:/fp:fast>")
  target_link_libraries(tnvse_font_quality_probe PRIVATE
    msdfgen_freetype_tnvse)
  tnvse_apply_msvc_defaults(tnvse_font_quality_probe)
  set_target_properties(tnvse_font_quality_probe PROPERTIES
    FOLDER "tNVSE/Diagnostics")
endif()

option(TNVSE_BUILD_DICTIONARY_UI_SYNTAX_TESTS
  "Build the deterministic dictionary UI syntax regression tests" OFF)
if(TNVSE_BUILD_DICTIONARY_UI_SYNTAX_TESTS)
  enable_testing()
  add_executable(tnvse_dictionary_ui_syntax_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/dictionary_ui_syntax_tests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/dictionary/dictionary_ui_syntax.cpp")
  target_compile_features(tnvse_dictionary_ui_syntax_tests PRIVATE cxx_std_20)
  target_include_directories(tnvse_dictionary_ui_syntax_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/dictionary")
  target_compile_options(tnvse_dictionary_ui_syntax_tests PRIVATE /utf-8)
  tnvse_apply_msvc_defaults(tnvse_dictionary_ui_syntax_tests)
  set_target_properties(tnvse_dictionary_ui_syntax_tests PROPERTIES
    FOLDER "tNVSE/Tests")
  add_test(NAME dictionary_ui_syntax
    COMMAND $<TARGET_FILE:tnvse_dictionary_ui_syntax_tests>)
endif()

option(TNVSE_BUILD_RENDERER_RESET_RECOVERY_TESTS
  "Build the deterministic renderer reset recovery state tests" OFF)
if(TNVSE_BUILD_RENDERER_RESET_RECOVERY_TESTS)
  enable_testing()
  add_executable(tnvse_renderer_reset_recovery_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/renderer_reset_recovery_tests.cpp")
  target_compile_features(tnvse_renderer_reset_recovery_tests PRIVATE cxx_std_20)
  target_include_directories(tnvse_renderer_reset_recovery_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font/native")
  target_compile_options(tnvse_renderer_reset_recovery_tests PRIVATE /utf-8)
  tnvse_apply_msvc_defaults(tnvse_renderer_reset_recovery_tests)
  set_target_properties(tnvse_renderer_reset_recovery_tests PROPERTIES
    FOLDER "tNVSE/Tests")
  add_test(NAME renderer_reset_recovery
    COMMAND $<TARGET_FILE:tnvse_renderer_reset_recovery_tests>)
endif()

option(TNVSE_BUILD_PREWARM_OVERLAY_MAILBOX_TESTS
  "Build the deterministic prewarm overlay mailbox/owner-state tests" OFF)
if(TNVSE_BUILD_PREWARM_OVERLAY_MAILBOX_TESTS)
  enable_testing()
  add_executable(tnvse_prewarm_overlay_mailbox_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prewarm_overlay_mailbox_tests.cpp")
  target_compile_features(tnvse_prewarm_overlay_mailbox_tests PRIVATE cxx_std_20)
  target_include_directories(tnvse_prewarm_overlay_mailbox_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/game")
  target_compile_definitions(tnvse_prewarm_overlay_mailbox_tests PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(tnvse_prewarm_overlay_mailbox_tests PRIVATE /utf-8)
  tnvse_apply_msvc_defaults(tnvse_prewarm_overlay_mailbox_tests)
  set_target_properties(tnvse_prewarm_overlay_mailbox_tests PROPERTIES
    FOLDER "tNVSE/Tests")
  add_test(NAME prewarm_overlay_mailbox
    COMMAND $<TARGET_FILE:tnvse_prewarm_overlay_mailbox_tests>)
endif()

option(TNVSE_BUILD_FONT_PREWARM_RUN_CONTROL_TESTS
  "Build the deterministic font-prewarm cancellation/watchdog tests" OFF)
if(TNVSE_BUILD_FONT_PREWARM_RUN_CONTROL_TESTS)
  enable_testing()
  add_executable(tnvse_font_prewarm_run_control_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/font_prewarm_run_control_tests.cpp")
  target_compile_features(tnvse_font_prewarm_run_control_tests PRIVATE cxx_std_20)
  target_include_directories(tnvse_font_prewarm_run_control_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font")
  target_compile_options(tnvse_font_prewarm_run_control_tests PRIVATE /utf-8)
  tnvse_apply_msvc_defaults(tnvse_font_prewarm_run_control_tests)
  set_target_properties(tnvse_font_prewarm_run_control_tests PROPERTIES
    FOLDER "tNVSE/Tests")
  add_test(NAME font_prewarm_run_control
    COMMAND $<TARGET_FILE:tnvse_font_prewarm_run_control_tests>)
endif()

option(TNVSE_BUILD_FONT_MULTIBYTE_PREWARM_TESTS
  "Build the deterministic multibyte prewarm policy tests" OFF)
if(TNVSE_BUILD_FONT_MULTIBYTE_PREWARM_TESTS)
  enable_testing()
  add_executable(tnvse_font_multibyte_prewarm_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/font_multibyte_prewarm_tests.cpp")
  target_compile_features(tnvse_font_multibyte_prewarm_tests PRIVATE cxx_std_20)
  target_include_directories(tnvse_font_multibyte_prewarm_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/font")
  target_compile_definitions(tnvse_font_multibyte_prewarm_tests PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(tnvse_font_multibyte_prewarm_tests PRIVATE /utf-8)
  tnvse_apply_msvc_defaults(tnvse_font_multibyte_prewarm_tests)
  set_target_properties(tnvse_font_multibyte_prewarm_tests PROPERTIES
    FOLDER "tNVSE/Tests")
  add_test(NAME font_multibyte_prewarm
    COMMAND $<TARGET_FILE:tnvse_font_multibyte_prewarm_tests>)
endif()

option(TNVSE_BUILD_TEXT_SAFETY_TESTS
  "Build the deterministic retail text-boundary regression tests" OFF)
if(TNVSE_BUILD_TEXT_SAFETY_TESTS)
  enable_testing()
  add_executable(tnvse_text_safety_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/text_safety_tests.cpp")
  target_compile_features(tnvse_text_safety_tests PRIVATE cxx_std_20)
  target_include_directories(tnvse_text_safety_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src")
  target_compile_options(tnvse_text_safety_tests PRIVATE /utf-8)
  tnvse_apply_msvc_defaults(tnvse_text_safety_tests)
  set_target_properties(tnvse_text_safety_tests PROPERTIES
    FOLDER "tNVSE/Tests")
  add_test(NAME text_safety
    COMMAND $<TARGET_FILE:tnvse_text_safety_tests>)
endif()

set(TNVSE_NATIVE_OVERLAY_XMLS
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Menus/prefabs/tNVSE/ImeOverlay.xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Menus/prefabs/tNVSE/FontPrewarmOverlay.xml")
set(TNVSE_NATIVE_OVERLAY_TEXTURES
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Textures/Interface/tNVSE/font_prewarm_label.dds")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Menus/prefabs/tNVSE/FontPrewarmOverlay.xml")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/docs/Menus/prefabs/tNVSE/FontPrewarmOverlay.xml"
  _tnvse_prewarm_overlay_xml)
foreach(_tnvse_forbidden_prewarm_tag text font string)
  if(_tnvse_prewarm_overlay_xml MATCHES
      "<[ \t\r\n]*${_tnvse_forbidden_prewarm_tag}([ \t\r\n>])")
    message(FATAL_ERROR
      "FontPrewarmOverlay.xml must remain TileImage-only; forbidden tag: ${_tnvse_forbidden_prewarm_tag}")
  endif()
endforeach()
if(NOT _tnvse_prewarm_overlay_xml MATCHES
    "<_tnvse_prewarm_progress>[ \t\r\n]*0[ \t\r\n]*</_tnvse_prewarm_progress>")
  message(FATAL_ERROR
    "FontPrewarmOverlay.xml must define root _tnvse_prewarm_progress=0")
endif()
if(NOT _tnvse_prewarm_overlay_xml MATCHES
    "<mul[ \t\r\n]+src=\"parent\"[ \t\r\n]+trait=\"_tnvse_prewarm_progress\"")
  message(FATAL_ERROR
    "FontPrewarmOverlay.xml fill width must be driven by the root progress trait")
endif()
unset(_tnvse_forbidden_prewarm_tag)
unset(_tnvse_prewarm_overlay_xml)
set(_tnvse_prewarm_overlay_cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/game/native_tile_overlay_prewarm.cpp")
set(_tnvse_loading_menu_hooks_cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src/game/native_tile_overlay_hooks.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_tnvse_prewarm_overlay_cpp}"
  "${_tnvse_loading_menu_hooks_cpp}")
file(READ "${_tnvse_prewarm_overlay_cpp}" _tnvse_prewarm_overlay_source)
file(READ "${_tnvse_loading_menu_hooks_cpp}" _tnvse_loading_menu_hooks_source)
foreach(_tnvse_forbidden_overlay_pattern
    "Sleep[ \t\r\n]*\\("
    "ReleaseAndDestroyAttachedRoot[ \t\r\n]*\\("
    "SetText[ \t\r\n]*\\("
    "RebuildTextGeometry[ \t\r\n]*\\("
    "ScopedFreeTypeNoPrecacheRoute")
  if(_tnvse_prewarm_overlay_source MATCHES
      "${_tnvse_forbidden_overlay_pattern}")
    message(FATAL_ERROR
      "Prewarm overlay owner path contains forbidden operation: ${_tnvse_forbidden_overlay_pattern}")
  endif()
endforeach()
foreach(_tnvse_removed_loading_menu_symbol
    "LoadingMenuShowChangesHook"
    "kLoadingMenuThreadShowChangesCallSite"
    "TryEnterCriticalSection"
    "OwnerThreadShutdownLatch")
  if(_tnvse_loading_menu_hooks_source MATCHES
      "${_tnvse_removed_loading_menu_symbol}")
    message(FATAL_ERROR
      "Removed LoadingMenu wrapper/lock remains in hook implementation: ${_tnvse_removed_loading_menu_symbol}")
  endif()
endforeach()
unset(_tnvse_forbidden_overlay_pattern)
unset(_tnvse_removed_loading_menu_symbol)
unset(_tnvse_prewarm_overlay_source)
unset(_tnvse_loading_menu_hooks_source)
unset(_tnvse_prewarm_overlay_cpp)
unset(_tnvse_loading_menu_hooks_cpp)
set_source_files_properties(
  ${TNVSE_NATIVE_OVERLAY_XMLS}
  ${TNVSE_NATIVE_OVERLAY_TEXTURES}
  PROPERTIES
  HEADER_FILE_ONLY TRUE
  VS_TOOL_OVERRIDE "None")

set(TNVSE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/Src")

file(GLOB_RECURSE TNVSE_SOURCES CONFIGURE_DEPENDS
  "${TNVSE_SOURCE_ROOT}/*.cpp")
file(GLOB_RECURSE TNVSE_SOURCE_HEADERS CONFIGURE_DEPENDS
  "${TNVSE_SOURCE_ROOT}/*.h")
file(GLOB_RECURSE TNVSE_BUNDLED_HEADERS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/include/*.hpp")
set(TNVSE_HEADERS ${TNVSE_SOURCE_HEADERS} ${TNVSE_BUNDLED_HEADERS})

# This translation unit intentionally keeps its user-facing Chinese text as
# editable UTF-8 source.  Decode only this file as UTF-8 so the rest of the
# plugin retains the existing multi-byte project/compiler settings.
set_source_files_properties(
  "${TNVSE_SOURCE_ROOT}/game/tianmiao_conflict.cpp"
  PROPERTIES COMPILE_OPTIONS "/source-charset:utf-8")

add_library(tnvse SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/dllmain.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/exports.def"
  ${TNVSE_SOURCES}
  ${TNVSE_HEADERS}
  ${TNVSE_NATIVE_OVERLAY_XMLS}
  ${TNVSE_NATIVE_OVERLAY_TEXTURES})

source_group("Source Files" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/dllmain.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/exports.def"
  ${TNVSE_SOURCES}
  ${TNVSE_SOURCE_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/include"
  PREFIX "Bundled Headers"
  FILES ${TNVSE_BUNDLED_HEADERS})
source_group("UI Overlays" FILES ${TNVSE_NATIVE_OVERLAY_XMLS})
source_group("UI Overlay Textures" FILES ${TNVSE_NATIVE_OVERLAY_TEXTURES})

target_include_directories(tnvse PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse"
  "${TNVSE_SOURCE_ROOT}"
  "${TNVSE_SOURCE_ROOT}/dictionary"
  "${TNVSE_SOURCE_ROOT}/font"
	"${TNVSE_SOURCE_ROOT}/font/native_shape"
  "${TNVSE_SOURCE_ROOT}/font/atlas"
  "${TNVSE_SOURCE_ROOT}/font/native"
  "${TNVSE_SOURCE_ROOT}/font/vector"
  "${TNVSE_SOURCE_ROOT}/game"
  "${TNVSE_SOURCE_ROOT}/input"
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/include")

target_compile_definitions(tnvse PRIVATE
  SOL_ALL_SAFETIES_ON
  SOL_SAFE_REFERENCES
  SOL_SAFE_FUNCTION_CALLS
  SOL_PRINT_ERRORS
  SOL_USING_CXX_LUA_JIT
  WIN32
  _WINDOWS
  _USRDLL
  _MBCS
  NVSE_PLUGIN_EXAMPLE_EXPORTS
  RUNTIME_VERSION=0x040020D0
  RUNTIME
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  "$<$<CONFIG:Debug>:_DEBUG>"
  "$<$<CONFIG:Release>:NDEBUG>")

target_compile_options(tnvse PRIVATE
  /EHa
  /Zc:preprocessor
  /bigobj
  /wd4244
  /FIPrefix.h
  /FIITypes.h
  /FIMemory.h
  /FIIErrors.h
  "$<$<COMPILE_LANGUAGE:C>:/std:c17>"
  "$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>"
  "$<$<CONFIG:Release>:/Ot;/fp:fast>")

target_link_libraries(tnvse PRIVATE
  commonlib_nv
  freetype_tnvse
  msdfgen_freetype_tnvse
  tnvse::dxsdk
  dxguid
  imm32
  ole32
  oleaut32
  uuid)

target_link_options(tnvse PRIVATE
  "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/tnvse/exports.def"
  /SUBSYSTEM:WINDOWS
  /DYNAMICBASE:NO
  /DEBUG
  "$<$<CONFIG:Debug>:/SAFESEH:NO>"
  "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>")

set_property(TARGET tnvse APPEND PROPERTY
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/exports.def")
tnvse_apply_msvc_defaults(tnvse)
set_property(TARGET tnvse PROPERTY
  MSVC_DEBUG_INFORMATION_FORMAT
  "$<$<CONFIG:Debug>:EditAndContinue>$<$<CONFIG:Release>:ProgramDatabase>")
set_target_properties(tnvse PROPERTIES
  OUTPUT_NAME "tnvse"
  FOLDER "tNVSE")

add_dependencies(tnvse tnvse_shaders)
tnvse_copy_dxsdk_runtime(tnvse)

set(_tnvse_plugin_path_help
  "Existing NVSE/plugins directory used for post-build deployment")
if(DEFINED ENV{NVSE_PLUGIN_PATH} AND NOT "$ENV{NVSE_PLUGIN_PATH}" STREQUAL "")
  # The Windows environment variable is authoritative on every configure.
  # FORCE prevents an older CMake cache entry from silently deploying to a
  # stale MO2 instance after NVSE_PLUGIN_PATH changes.
  file(TO_CMAKE_PATH "$ENV{NVSE_PLUGIN_PATH}" _tnvse_plugin_path_from_env)
  set(TNVSE_PLUGIN_PATH "${_tnvse_plugin_path_from_env}" CACHE PATH
    "${_tnvse_plugin_path_help}" FORCE)
else()
  # Without the environment variable, retain support for an explicit
  # -DTNVSE_PLUGIN_PATH=... cache value.
  set(TNVSE_PLUGIN_PATH "" CACHE PATH "${_tnvse_plugin_path_help}")
endif()
unset(_tnvse_plugin_path_from_env)
unset(_tnvse_plugin_path_help)
tnvse_enable_live_deployment(tnvse "${TNVSE_PLUGIN_PATH}"
  "${TNVSE_SHADER_OUTPUTS}" "${TNVSE_NATIVE_OVERLAY_XMLS}"
  "${TNVSE_NATIVE_OVERLAY_TEXTURES}")

install(TARGETS tnvse RUNTIME DESTINATION "NVSE/plugins")
install(FILES ${TNVSE_SHADER_OUTPUTS} DESTINATION "Shaders/Loose")
install(CODE [[
  file(REMOVE
    "${CMAKE_INSTALL_PREFIX}/Menus/prefabs/tNVSE/NativeOverlays.xml")
]])
install(FILES ${TNVSE_NATIVE_OVERLAY_XMLS}
  DESTINATION "Menus/prefabs/tNVSE")
install(FILES ${TNVSE_NATIVE_OVERLAY_TEXTURES}
  DESTINATION "Textures/Interface/tNVSE")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/tnvse_fonts.xml"
  DESTINATION "NVSE/plugins")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/fonts/"
  DESTINATION "NVSE/plugins/fonts"
  FILES_MATCHING
    PATTERN "*.ttf"
    PATTERN "*.ttc"
    PATTERN "*.otf")
