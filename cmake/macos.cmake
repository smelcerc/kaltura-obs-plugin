set(KALTURA_LIVE_OBS_APP_PATH "/Applications/OBS.app" CACHE PATH "Path to OBS.app")
set(KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH
  "${KALTURA_LIVE_OBS_APP_PATH}/Contents/Frameworks" CACHE PATH
  "Path to the OBS.app Frameworks directory")
set(KALTURA_LIVE_PLATFORM_SOURCE src/platform/platform_macos.mm)
set(KALTURA_LIVE_PLATFORM_LIBRARIES "-framework Security" "-framework CoreFoundation")
set(KALTURA_LIVE_PLATFORM_DEFINITIONS KALTURA_PLATFORM_MACOS=1)

if(CMAKE_OSX_ARCHITECTURES MATCHES ";")
  message(FATAL_ERROR
    "whisper.cpp v1.9.1 selects CPU backends at configure time and cannot be built safely as a "
    "single multi-architecture target. Build x86_64 and arm64 separately and merge them with "
    "scripts/package-macos-universal.sh. See docs/BUILD_MACOS.md.")
endif()

function(kaltura_link_obs_fallback target)
  set(frameworks "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}")
  foreach(dependency IN ITEMS
      libobs.framework/Versions/A/libobs
      obs-frontend-api.dylib
      QtCore.framework/Versions/A/QtCore
      QtGui.framework/Versions/A/QtGui
      QtNetwork.framework/Versions/A/QtNetwork
      QtWidgets.framework/Versions/A/QtWidgets)
    if(NOT EXISTS "${frameworks}/${dependency}")
      message(FATAL_ERROR "Required OBS runtime library not found: ${frameworks}/${dependency}")
    endif()
  endforeach()
  target_include_directories(${target} PRIVATE
    $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Gui,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Network,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Widgets,INTERFACE_INCLUDE_DIRECTORIES>
    "${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs"
    "${KALTURA_LIVE_OBS_SOURCE_PATH}/frontend/api")
  target_compile_definitions(${target} PRIVATE
    QT_CORE_LIB QT_GUI_LIB QT_NETWORK_LIB QT_WIDGETS_LIB)
  target_link_options(${target} PRIVATE
    "${frameworks}/libobs.framework/Versions/A/libobs"
    "${frameworks}/obs-frontend-api.dylib"
    "${frameworks}/QtCore.framework/Versions/A/QtCore"
    "${frameworks}/QtGui.framework/Versions/A/QtGui"
    "${frameworks}/QtNetwork.framework/Versions/A/QtNetwork"
    "${frameworks}/QtWidgets.framework/Versions/A/QtWidgets")
endfunction()

function(kaltura_configure_platform_target target)
  set_target_properties(${target} PROPERTIES
    BUILD_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH "@executable_path/../Frameworks")
endfunction()

function(kaltura_configure_install target)
  # The macOS packaging script constructs the .plugin bundle. A bare CMake install is
  # still useful for staging and deliberately mirrors that bundle layout.
  install(TARGETS ${target} LIBRARY DESTINATION "kaltura-live.plugin/Contents/MacOS")
endfunction()

function(kaltura_configure_cpack)
  set(CPACK_GENERATOR "TGZ" PARENT_SCOPE)
  set(CPACK_PACKAGE_FILE_NAME
    "kaltura-live-${PROJECT_VERSION}-macOS-${CMAKE_OSX_ARCHITECTURES}" PARENT_SCOPE)
endfunction()
