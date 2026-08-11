set(KALTURA_LIVE_PLATFORM_SOURCE src/platform/platform_windows.cpp)
set(KALTURA_LIVE_PLATFORM_LIBRARIES Advapi32)
set(KALTURA_LIVE_PLATFORM_DEFINITIONS KALTURA_PLATFORM_WINDOWS=1 UNICODE _UNICODE)

function(kaltura_configure_platform_target target)
  set_target_properties(${target} PROPERTIES PREFIX "")
endfunction()

function(kaltura_configure_install target)
  install(TARGETS ${target}
    RUNTIME DESTINATION "obs-plugins/64bit"
    LIBRARY DESTINATION "obs-plugins/64bit")
  install(PROGRAMS packaging/windows/uninstall-kaltura-live.ps1 DESTINATION ".")
  if(KALTURA_LIVE_MODEL_SOURCE_DIR)
    foreach(model IN ITEMS ggml-tiny.en.bin ggml-base.en.bin)
      if(NOT EXISTS "${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}")
        message(FATAL_ERROR "Packaging model not found: ${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}")
      endif()
      install(FILES "${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}"
        DESTINATION "data/obs-plugins/kaltura-live/models")
    endforeach()
  endif()
endfunction()

function(kaltura_configure_cpack)
  set(CPACK_GENERATOR "ZIP" PARENT_SCOPE)
  set(CPACK_PACKAGE_FILE_NAME
    "kaltura-live-${PROJECT_VERSION}-Windows-x86_64" PARENT_SCOPE)
endfunction()
