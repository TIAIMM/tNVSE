include_guard(GLOBAL)

function(tnvse_apply_msvc_defaults target)
  set_property(TARGET ${target} PROPERTY
    MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>$<$<NOT:$<CONFIG:Debug>>:DLL>")
  set_property(TARGET ${target} PROPERTY
    MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
  set_property(TARGET ${target} PROPERTY
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

  target_compile_options(${target} PRIVATE
    /MP
    /W3
    "$<$<CONFIG:Debug>:/Od;/RTC1>"
    "$<$<CONFIG:Release>:/O2;/Gy;/Oi>")
endfunction()

