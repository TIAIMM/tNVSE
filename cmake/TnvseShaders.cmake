include_guard(GLOBAL)

function(tnvse_add_shader_target output_variable)
  set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/tnvse/shaders")
  set(shader_sources
    "${shader_dir}/freetype_native_common.hlsli"
    "${shader_dir}/freetype_native_vs.hlsl"
    "${shader_dir}/freetype_native_mtsdf_fill.hlsl"
    "${shader_dir}/freetype_native_mtsdf_effects.hlsl"
    "${shader_dir}/compile_a8_shader.bat"
    "${shader_dir}/verify_shader_contract.ps1")
  set(shader_outputs
    "${shader_dir}/compiled/tnvse_freetype_native_vs.vso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_fill_fast.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_fill_balanced.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_fill_high.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_fill_subpixel_fast.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_fill_subpixel_balanced.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_fill_subpixel_high.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_effects_fast.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_effects_balanced.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_mtsdf_effects_high.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_fill_fast.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_fill_balanced.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_fill_high.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_fill_subpixel_fast.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_fill_subpixel_balanced.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_fill_subpixel_high.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_effects_fast.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_effects_balanced.pso"
    "${shader_dir}/compiled/tnvse_freetype_native_sdf_effects_high.pso")

  # Keep HLSL visible in the generated Visual Studio solution without letting
  # VS invoke its generic FxCompile rule. The batch file is the authoritative
  # compiler because it supplies the per-shader profiles, defines, and ABI test.
  set_source_files_properties(${shader_sources} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None")

  add_custom_target(tnvse_shaders
    COMMAND "${shader_dir}/compile_a8_shader.bat"
    WORKING_DIRECTORY "${shader_dir}"
    BYPRODUCTS ${shader_outputs}
    SOURCES ${shader_sources}
    COMMENT "Compiling and verifying native FreeType true-SDF and MTSDF shaders"
    VERBATIM)
  set_target_properties(tnvse_shaders PROPERTIES FOLDER "tNVSE/Shaders")
  set(${output_variable} "${shader_outputs}" PARENT_SCOPE)
endfunction()

function(tnvse_enable_live_deployment target plugin_path shader_outputs)
  if(NOT plugin_path)
    message(STATUS "TNVSE_PLUGIN_PATH is empty; live MO2 deployment is disabled.")
    return()
  endif()

  get_filename_component(plugin_path "${plugin_path}" ABSOLUTE)
  get_filename_component(mod_root "${plugin_path}/../.." ABSOLUTE)
  set(shader_path "${mod_root}/Shaders/Loose")
  message(STATUS "Live tNVSE DLL deployment: ${plugin_path}")
  message(STATUS "Live tNVSE shader deployment: ${shader_path}")

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${plugin_path}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:${target}>" "${plugin_path}/$<TARGET_FILE_NAME:${target}>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_path}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
      "${shader_path}/tnvse_freetype_native_original.pso"
      "${shader_path}/tnvse_freetype_native_coverage.pso"
      "${shader_path}/tnvse_freetype_native_sdf.pso"
      "${shader_path}/tnvse_freetype_native_effects_fast.pso"
      "${shader_path}/tnvse_freetype_native_effects_balanced.pso"
      "${shader_path}/tnvse_freetype_native_effects_high.pso"
      "${shader_path}/tnvse_freetype_native_mtsdf_fill.pso"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      ${shader_outputs} "${shader_path}"
    COMMENT "Deploying tNVSE DLL and native shaders"
    VERBATIM)
endfunction()
