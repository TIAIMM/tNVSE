include_guard(GLOBAL)

function(tnvse_apply_msvc_defaults target)
  set_property(TARGET ${target} PROPERTY
    MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_property(TARGET ${target} PROPERTY
    MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
  set_property(TARGET ${target} PROPERTY
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

  if(NOT TARGET tnvse::vcltl)
    message(FATAL_ERROR
      "tnvse::vcltl must be created before applying the MSVC defaults.")
  endif()
  target_link_libraries(${target} PRIVATE tnvse::vcltl)

  target_compile_options(${target} PRIVATE
    /MP
    /W3
    "$<$<CONFIG:Debug>:/Od;/RTC1>"
    "$<$<CONFIG:Release>:/O2;/Gy;/Oi>")
endfunction()
