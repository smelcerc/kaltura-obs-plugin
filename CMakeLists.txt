cmake_minimum_required(VERSION 3.28)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" KALTURA_LIVE_VERSION)
string(STRIP "${KALTURA_LIVE_VERSION}" KALTURA_LIVE_VERSION)
if(NOT KALTURA_LIVE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([.-][0-9A-Za-z.-]+)?$")
  message(FATAL_ERROR "VERSION must contain a semantic version")
endif()

project(kaltura-live
  VERSION ${KALTURA_LIVE_VERSION}
  DESCRIPTION "Kaltura Live plugin for OBS Studio"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(GNUInstallDirs)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(
    "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=.")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_Declare(whisper_cpp
  GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(whisper_cpp)
add_subdirectory(third_party/obs-studio/deps/libcaption EXCLUDE_FROM_ALL)

find_package(libobs QUIET)
find_package(obs-frontend-api QUIET)

set(KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH "/Applications/OBS.app/Contents/Frameworks" CACHE PATH
  "Path to OBS.app Frameworks directory")
set(KALTURA_LIVE_OBS_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-studio" CACHE PATH
  "Path to OBS Studio source tree used for headers in fallback mode")
set(KALTURA_LIVE_MODEL_SOURCE_DIR "" CACHE PATH
  "Optional directory containing ggml-tiny.en.bin and ggml-base.en.bin for packages")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/kaltura_live/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/kaltura_live/version.hpp
  @ONLY)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/kaltura_live/obsconfig.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h
  @ONLY)

add_library(kaltura-api-client STATIC
  src/api/kaltura_api_client.cpp
  src/api/qt_http_transport.cpp)

set_target_properties(kaltura-api-client PROPERTIES AUTOMOC OFF)

target_include_directories(kaltura-api-client
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Network,INTERFACE_INCLUDE_DIRECTORIES>)

target_compile_definitions(kaltura-api-client
  PRIVATE
    QT_CORE_LIB
    QT_NETWORK_LIB
    QT_NO_KEYWORDS)

if(MSVC)
  target_compile_options(kaltura-api-client PRIVATE /W4 /permissive-)
else()
  target_compile_options(kaltura-api-client PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(${PROJECT_NAME} MODULE
  src/plugin-main.cpp
  src/plugin.cpp
  src/logger.cpp
  src/settings_manager.cpp
  src/settings_dialog.cpp
  src/streaming_manager.cpp
  src/captions/caption_manager.cpp
  src/captions/cea608_caption_inserter.cpp
  src/captions/whisper_provider.cpp
  src/kaltura_dock.cpp
  resources/resources.qrc)

target_link_libraries(${PROJECT_NAME} PRIVATE
  kaltura-api-client
  OBS::caption
  whisper)

target_include_directories(${PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)

if(TARGET OBS::libobs AND TARGET OBS::obs-frontend-api)
  target_link_libraries(${PROJECT_NAME}
    PRIVATE
      Qt6::Core
      Qt6::Network
      Qt6::Widgets
      OBS::libobs
      OBS::obs-frontend-api)
elseif(APPLE)
  message(WARNING "Using macOS fallback OBS linkage. Prefer libobs/obs-frontend-api CMake packages when available.")

  if(NOT EXISTS "${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs/obs-module.h")
    message(FATAL_ERROR
      "Fallback mode requires OBS source headers at ${KALTURA_LIVE_OBS_SOURCE_PATH}. "
      "Clone obsproject/obs-studio there or set KALTURA_LIVE_OBS_SOURCE_PATH.")
  endif()

  if(NOT EXISTS "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/libobs.framework")
    message(FATAL_ERROR
      "Fallback mode requires libobs.framework in ${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}. "
      "Install OBS.app or set KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH.")
  endif()

  target_include_directories(${PROJECT_NAME}
    PRIVATE
      $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>
      $<TARGET_PROPERTY:Qt6::Gui,INTERFACE_INCLUDE_DIRECTORIES>
      $<TARGET_PROPERTY:Qt6::Network,INTERFACE_INCLUDE_DIRECTORIES>
      $<TARGET_PROPERTY:Qt6::Widgets,INTERFACE_INCLUDE_DIRECTORIES>
      ${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs
      ${KALTURA_LIVE_OBS_SOURCE_PATH}/frontend/api)

  # Passing app-bundled frameworks through target_link_libraries adds OBS's
  # Frameworks directory to compile commands. OBS strips Qt headers from its
  # bundle, so Clang then finds a headerless QtWidgets.framework before the Qt
  # development installation. Keep these as link-only inputs instead.
  target_link_options(${PROJECT_NAME}
    PRIVATE
      "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/libobs.framework/Versions/A/libobs"
      "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/obs-frontend-api.dylib"
      "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/QtCore.framework/Versions/A/QtCore"
      "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/QtGui.framework/Versions/A/QtGui"
      "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/QtNetwork.framework/Versions/A/QtNetwork"
      "${KALTURA_LIVE_OBS_APP_FRAMEWORK_PATH}/QtWidgets.framework/Versions/A/QtWidgets")

  target_compile_definitions(${PROJECT_NAME}
    PRIVATE
      QT_CORE_LIB
      QT_GUI_LIB
      QT_NETWORK_LIB
      QT_WIDGETS_LIB)

  set_target_properties(${PROJECT_NAME}
    PROPERTIES
      BUILD_RPATH "@executable_path/../Frameworks"
      INSTALL_RPATH "@executable_path/../Frameworks")
else()
  message(FATAL_ERROR
    "Could not find libobs and obs-frontend-api CMake packages. "
    "Install OBS dev packages or use the macOS fallback mode.")
endif()

target_compile_definitions(${PROJECT_NAME}
  PRIVATE
    QT_NO_KEYWORDS)

if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive-)
else()
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(${PROJECT_NAME}
  PROPERTIES
    OUTPUT_NAME "kaltura-live")

if(NOT APPLE)
  install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)
  if(KALTURA_LIVE_MODEL_SOURCE_DIR)
    foreach(model IN ITEMS ggml-tiny.en.bin ggml-base.en.bin)
      if(NOT EXISTS "${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}")
        message(FATAL_ERROR "Packaging model not found: ${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}")
      endif()
      install(FILES "${KALTURA_LIVE_MODEL_SOURCE_DIR}/${model}"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/kaltura-live/models)
    endforeach()
  endif()
  install(FILES README.md docs/USER_GUIDE.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(kaltura-api-client-tests
    tests/kaltura_api_client_tests.cpp)
  set_target_properties(kaltura-api-client-tests PROPERTIES AUTOMOC OFF)
  target_link_libraries(kaltura-api-client-tests
    PRIVATE
      kaltura-api-client
      Qt6::Core
      Qt6::Network)
  target_compile_definitions(kaltura-api-client-tests
    PRIVATE
      KALTURA_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME kaltura-api-client-tests COMMAND kaltura-api-client-tests)

  add_executable(kaltura-caption-provider-tests
    tests/caption_provider_tests.cpp
    src/captions/whisper_provider.cpp)
  set_target_properties(kaltura-caption-provider-tests PROPERTIES AUTOMOC OFF)
  target_include_directories(kaltura-caption-provider-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(kaltura-caption-provider-tests PRIVATE
    whisper)
  add_test(NAME kaltura-caption-provider-tests COMMAND kaltura-caption-provider-tests)

  add_executable(kaltura-cea608-caption-tests
    tests/cea608_caption_inserter_tests.cpp
    src/captions/cea608_caption_inserter.cpp)
  set_target_properties(kaltura-cea608-caption-tests PROPERTIES AUTOMOC OFF)
  target_include_directories(kaltura-cea608-caption-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs)
  target_link_libraries(kaltura-cea608-caption-tests PRIVATE Qt6::Core)
  add_test(NAME kaltura-cea608-caption-tests COMMAND kaltura-cea608-caption-tests)
endif()

set(CPACK_PACKAGE_NAME "kaltura-live")
set(CPACK_PACKAGE_VENDOR "Kaltura Live Plugin Contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Kaltura Live streaming and local captions for OBS Studio")
set(CPACK_PACKAGE_CONTACT "Kaltura Live Plugin Maintainers")
set(CPACK_PACKAGE_FILE_NAME
  "kaltura-live-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)
if(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION "video")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "obs-studio (>= 31)")
else()
  set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
