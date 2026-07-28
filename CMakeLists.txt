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
set(TNVSE_NATIVE_OVERLAY_XMLS
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Menus/prefabs/tNVSE/ImeOverlay.xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/docs/Menus/prefabs/tNVSE/FontPrewarmOverlay.xml")
set_source_files_properties(${TNVSE_NATIVE_OVERLAY_XMLS} PROPERTIES
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

add_library(tnvse SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/dllmain.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/exports.def"
  ${TNVSE_SOURCES}
  ${TNVSE_HEADERS}
  ${TNVSE_NATIVE_OVERLAY_XMLS})

source_group("Source Files" FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/dllmain.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/exports.def"
  ${TNVSE_SOURCES}
  ${TNVSE_SOURCE_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/include"
  PREFIX "Bundled Headers"
  FILES ${TNVSE_BUNDLED_HEADERS})
source_group("UI Overlays" FILES ${TNVSE_NATIVE_OVERLAY_XMLS})

target_include_directories(tnvse PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse"
  "${TNVSE_SOURCE_ROOT}"
  "${TNVSE_SOURCE_ROOT}/dictionary"
  "${TNVSE_SOURCE_ROOT}/font"
  "${TNVSE_SOURCE_ROOT}/font/a8"
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
  "${TNVSE_SHADER_OUTPUTS}" "${TNVSE_NATIVE_OVERLAY_XMLS}")

install(TARGETS tnvse RUNTIME DESTINATION "NVSE/plugins")
install(FILES ${TNVSE_SHADER_OUTPUTS} DESTINATION "Shaders/Loose")
install(CODE [[
  file(REMOVE
    "${CMAKE_INSTALL_PREFIX}/Menus/prefabs/tNVSE/NativeOverlays.xml")
]])
install(FILES ${TNVSE_NATIVE_OVERLAY_XMLS}
  DESTINATION "Menus/prefabs/tNVSE")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/tnvse_fonts.xml"
  DESTINATION "NVSE/plugins")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/fonts/"
  DESTINATION "NVSE/plugins/fonts"
  FILES_MATCHING
    PATTERN "*.ttf"
    PATTERN "*.ttc"
    PATTERN "*.otf")
