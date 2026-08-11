find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

set(KALTURA_LIVE_PLATFORM_SOURCE src/platform/platform_linux.cpp)
set(KALTURA_LIVE_PLATFORM_LIBRARIES PkgConfig::LIBSECRET)
set(KALTURA_LIVE_PLATFORM_DEFINITIONS KALTURA_PLATFORM_LINUX=1 KALTURA_HAVE_LIBSECRET=1)

function(kaltura_configure_platform_target target)
endfunction()

function(kaltura_configure_install target)
  install(TARGETS ${target} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
  if(KALTURA_LIVE_MODEL_SOURCE_DIR)
    foreach(model IN ITEMS ggml-tiny.en.bin ggml-base.en.bin)
      if(NOT EXISTS "${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}")
        message(FATAL_ERROR "Packaging model not found: ${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}")
      endif()
      install(FILES "${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/kaltura-live/models")
    endforeach()
  endif()
  install(FILES README.md docs/USER_GUIDE.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
  install(PROGRAMS packaging/linux/uninstall-kaltura-live.sh
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
endfunction()

function(kaltura_configure_cpack)
  set(CPACK_GENERATOR "DEB;TGZ" PARENT_SCOPE)
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr" PARENT_SCOPE)
  set(CPACK_PACKAGE_FILE_NAME
    "kaltura-live-${PROJECT_VERSION}-Linux-x86_64" PARENT_SCOPE)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT PARENT_SCOPE)
  set(CPACK_DEBIAN_PACKAGE_SECTION "video" PARENT_SCOPE)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON PARENT_SCOPE)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "obs-studio (>= 32.0), libsecret-1-0" PARENT_SCOPE)
endfunction()
