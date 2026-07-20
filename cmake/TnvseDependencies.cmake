include_guard(GLOBAL)

set(TNVSE_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/third_party")
set(TNVSE_DXSDK_PACKAGE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/packages/Microsoft.DXSDK.D3DX.9.29.952.8/build/native")
set(TNVSE_VCLTL_VERSION "5.3.1")
set(TNVSE_VCLTL_TARGET_VERSION "6.0.6000.0")
set(TNVSE_VCLTL_PACKAGE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/packages/VC-LTL.${TNVSE_VCLTL_VERSION}/build/native")

function(tnvse_add_vcltl_target)
  set(vcltl_target_root
    "${TNVSE_VCLTL_PACKAGE_DIR}/TargetPlatform/${TNVSE_VCLTL_TARGET_VERSION}")
  set(vcltl_library_dir "${vcltl_target_root}/lib/Win32")

  foreach(required_file IN ITEMS
      "${TNVSE_VCLTL_PACKAGE_DIR}/TargetPlatform/header/corecrt.h"
      "${vcltl_target_root}/header/vcruntime.h"
      "${vcltl_library_dir}/libucrt.lib"
      "${vcltl_library_dir}/libvcruntime.lib")
    if(NOT EXISTS "${required_file}")
      message(FATAL_ERROR
        "VC-LTL ${TNVSE_VCLTL_VERSION} NuGet files are missing. "
        "Run: nuget restore tnvse/packages.config -PackagesDirectory tnvse/packages")
    endif()
  endforeach()

  add_library(tnvse_vcltl INTERFACE)
  add_library(tnvse::vcltl ALIAS tnvse_vcltl)
  target_include_directories(tnvse_vcltl BEFORE INTERFACE
    "${TNVSE_VCLTL_PACKAGE_DIR}/TargetPlatform/header"
    "${vcltl_target_root}/header")
  target_link_directories(tnvse_vcltl BEFORE INTERFACE
    "${vcltl_library_dir}")
  target_compile_definitions(tnvse_vcltl INTERFACE
    _Build_By_LTL=1
    _LTL_Core_Version=5)
endfunction()

function(tnvse_add_dxsdk_target)
  if(NOT EXISTS "${TNVSE_DXSDK_PACKAGE_DIR}/include/d3dx9.h")
    message(FATAL_ERROR
      "Microsoft.DXSDK.D3DX NuGet files are missing. Restore tnvse/packages before configuring CMake.")
  endif()

  add_library(tnvse_dxsdk INTERFACE)
  add_library(tnvse::dxsdk ALIAS tnvse_dxsdk)
  target_include_directories(tnvse_dxsdk INTERFACE
    "${TNVSE_DXSDK_PACKAGE_DIR}/include")
  target_compile_definitions(tnvse_dxsdk INTERFACE HAS_DXSDK_D3DX)
  target_link_directories(tnvse_dxsdk BEFORE INTERFACE
    "$<$<CONFIG:Debug>:${TNVSE_DXSDK_PACKAGE_DIR}/debug/lib/x86>"
    "$<$<NOT:$<CONFIG:Debug>>:${TNVSE_DXSDK_PACKAGE_DIR}/release/lib/x86>")
  target_link_libraries(tnvse_dxsdk INTERFACE
    debug d3dx9d optimized d3dx9
    debug d3dx10d optimized d3dx10
    debug d3dx11d optimized d3dx11)
endfunction()

function(tnvse_add_commonlib_target)
  file(GLOB_RECURSE COMMONLIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Src/*.cpp")
  file(GLOB_RECURSE COMMONLIB_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include/*.hpp")

  add_library(commonlib_nv STATIC ${COMMONLIB_SOURCES} ${COMMONLIB_HEADERS})
  source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv"
    PREFIX "commonlib_nv"
    FILES ${COMMONLIB_SOURCES} ${COMMONLIB_HEADERS})
  target_include_directories(commonlib_nv PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include"
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include/Bethesda"
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include/Gamebryo"
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include/Havok"
    "${CMAKE_CURRENT_SOURCE_DIR}/commonlib_nv/Include/Utils")
  target_compile_features(commonlib_nv PRIVATE cxx_std_20)
  target_compile_definitions(commonlib_nv PRIVATE
    NOMINMAX WIN32 _WINDOWS _LIB _MBCS
    "$<$<CONFIG:Debug>:_DEBUG>"
    "$<$<CONFIG:Release>:NDEBUG>")
  target_compile_options(commonlib_nv PRIVATE
    /permissive-
    /sdl
    /wd4244
    /FIITypes.h
    /FIMemory.h
    /FIIErrors.h
    /FIPrefix.h)
  target_link_libraries(commonlib_nv PUBLIC tnvse::dxsdk)
  tnvse_apply_msvc_defaults(commonlib_nv)
  set_target_properties(commonlib_nv PROPERTIES FOLDER "tNVSE/Dependencies")
endfunction()

function(tnvse_add_freetype_target)
  set(ft "${TNVSE_THIRD_PARTY_DIR}/freetype-2.14.3")
  set(ft_sources
    "${ft}/src/autofit/autofit.c"
    "${ft}/src/base/ftbase.c"
    "${ft}/src/base/ftbitmap.c"
    "${ft}/src/base/ftglyph.c"
    "${ft}/src/base/ftinit.c"
    "${ft}/src/base/ftmm.c"
    "${ft}/src/base/ftstroke.c"
    "${ft}/src/cff/cff.c"
    "${ft}/src/psaux/psaux.c"
    "${ft}/src/pshinter/pshinter.c"
    "${ft}/src/psnames/psmodule.c"
    "${ft}/src/sfnt/sfnt.c"
    "${ft}/src/sdf/sdf.c"
    "${ft}/src/smooth/smooth.c"
    "${ft}/src/truetype/truetype.c"
    "${ft}/builds/windows/ftdebug.c"
    "${ft}/builds/windows/ftsystem.c")

  add_library(freetype_tnvse STATIC ${ft_sources}
    "${TNVSE_THIRD_PARTY_DIR}/freetype_config/tnvse_ftoption.h"
    "${TNVSE_THIRD_PARTY_DIR}/freetype_config/tnvse_ftmodule.h")
  target_include_directories(freetype_tnvse
    PUBLIC "${ft}/include"
    PRIVATE "${TNVSE_THIRD_PARTY_DIR}/freetype_config")
  target_compile_definitions(freetype_tnvse PRIVATE
    WIN32 _LIB _CRT_SECURE_NO_WARNINGS FT2_BUILD_LIBRARY
    "FT_CONFIG_OPTIONS_H=<tnvse_ftoption.h>"
    "FT_CONFIG_MODULES_H=<tnvse_ftmodule.h>"
    "$<$<CONFIG:Debug>:_DEBUG>"
    "$<$<CONFIG:Release>:NDEBUG>")
  target_compile_options(freetype_tnvse PRIVATE
    /utf-8 /wd4001 /wd4244 /wd4267)
  tnvse_apply_msvc_defaults(freetype_tnvse)
  set_target_properties(freetype_tnvse PROPERTIES FOLDER "tNVSE/Dependencies")
endfunction()

function(tnvse_add_libunibreak_target)
  set(ub "${TNVSE_THIRD_PARTY_DIR}/libunibreak/src")
  set(ub_sources
    "${ub}/linebreak.c"
    "${ub}/linebreakdata.c"
    "${ub}/linebreakdef.c"
    "${ub}/wordbreak.c"
    "${ub}/graphemebreak.c"
    "${ub}/eastasianwidthdef.c"
    "${ub}/emojidef.c"
    "${ub}/unibreakbase.c"
    "${ub}/unibreakdef.c")
  add_library(libunibreak_tnvse STATIC ${ub_sources})
  target_include_directories(libunibreak_tnvse PUBLIC "${ub}")
  target_compile_definitions(libunibreak_tnvse PRIVATE
    WIN32 _LIB
    "$<$<CONFIG:Debug>:_DEBUG>"
    "$<$<CONFIG:Release>:NDEBUG>")
  target_compile_options(libunibreak_tnvse PRIVATE /utf-8 /bigobj)
  tnvse_apply_msvc_defaults(libunibreak_tnvse)
  set_target_properties(libunibreak_tnvse PROPERTIES FOLDER "tNVSE/Dependencies")
endfunction()

function(tnvse_copy_dxsdk_runtime target)
  set(debug_bin "${TNVSE_DXSDK_PACKAGE_DIR}/debug/bin/x86")
  set(release_bin "${TNVSE_DXSDK_PACKAGE_DIR}/release/bin/x86")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<IF:$<CONFIG:Debug>,${debug_bin}/D3DCompiler_43.dll,${release_bin}/D3DCompiler_43.dll>"
      "$<TARGET_FILE_DIR:${target}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<IF:$<CONFIG:Debug>,${debug_bin}/D3dx9d_43.dll,${release_bin}/d3dx9_43.dll>"
      "$<TARGET_FILE_DIR:${target}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<IF:$<CONFIG:Debug>,${debug_bin}/D3DX10d_43.dll,${release_bin}/d3dx10_43.dll>"
      "$<TARGET_FILE_DIR:${target}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<IF:$<CONFIG:Debug>,${debug_bin}/D3DX11d_43.dll,${release_bin}/d3dx11_43.dll>"
      "$<TARGET_FILE_DIR:${target}>"
    VERBATIM)
endfunction()
