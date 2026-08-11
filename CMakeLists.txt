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

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(
    "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=.")
endif()

if(APPLE)
  include(macos)
elseif(WIN32)
  include(windows)
elseif(UNIX AND NOT APPLE)
  include(linux)
else()
  message(FATAL_ERROR "Kaltura Live supports macOS, Windows, and Linux only")
endif()

include(dependencies)

configure_file(
  include/kaltura_live/version.hpp.in
  generated/kaltura_live/version.hpp
  @ONLY)
configure_file(
  include/kaltura_live/obsconfig.h.in
  generated/obsconfig.h
  @ONLY)

add_library(kaltura-api-client STATIC
  src/api/kaltura_api_client.cpp
  src/api/qt_http_transport.cpp)
set_target_properties(kaltura-api-client PROPERTIES AUTOMOC OFF)
target_include_directories(kaltura-api-client PUBLIC include)
if(APPLE AND NOT (TARGET OBS::libobs AND TARGET OBS::obs-frontend-api))
  target_include_directories(kaltura-api-client PRIVATE
    $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Qt6::Network,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(kaltura-api-client PRIVATE QT_CORE_LIB QT_NETWORK_LIB)
else()
  target_link_libraries(kaltura-api-client PUBLIC Qt6::Core Qt6::Network)
endif()
target_compile_definitions(kaltura-api-client PRIVATE QT_NO_KEYWORDS)

add_library(kaltura-platform STATIC ${KALTURA_LIVE_PLATFORM_SOURCE})
target_include_directories(kaltura-platform PUBLIC include)
if(APPLE AND NOT (TARGET OBS::libobs AND TARGET OBS::obs-frontend-api))
  target_include_directories(kaltura-platform PRIVATE
    $<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>)
  target_link_libraries(kaltura-platform PUBLIC ${KALTURA_LIVE_PLATFORM_LIBRARIES})
  target_compile_definitions(kaltura-platform PRIVATE QT_CORE_LIB)
else()
  target_link_libraries(kaltura-platform PUBLIC Qt6::Core ${KALTURA_LIVE_PLATFORM_LIBRARIES})
endif()
target_compile_definitions(kaltura-platform PRIVATE ${KALTURA_LIVE_PLATFORM_DEFINITIONS})
target_compile_definitions(kaltura-platform PRIVATE QT_NO_KEYWORDS)

add_library(${PROJECT_NAME} MODULE
  src/plugin-main.cpp
  src/plugin.cpp
  src/logger.cpp
  src/settings_manager.cpp
  src/settings_dialog.cpp
  src/kaltura_player_embed.cpp
  src/stream_output_config.cpp
  src/streaming_manager.cpp
  src/captions/caption_manager.cpp
  src/captions/cea608_caption_inserter.cpp
  src/captions/whisper_provider.cpp
  src/kaltura_dock.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
  include
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(${PROJECT_NAME} PRIVATE
  kaltura-api-client
  kaltura-platform
  OBS::caption
  whisper)
if(NOT (APPLE AND NOT (TARGET OBS::libobs AND TARGET OBS::obs-frontend-api)))
  target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Network Qt6::Widgets)
endif()
kaltura_configure_obs_target(${PROJECT_NAME})
target_compile_definitions(${PROJECT_NAME} PRIVATE QT_NO_KEYWORDS)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "kaltura-live")

foreach(target IN ITEMS kaltura-api-client kaltura-platform ${PROJECT_NAME})
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

kaltura_configure_platform_target(${PROJECT_NAME})
kaltura_configure_install(${PROJECT_NAME})

include(CTest)
if(BUILD_TESTING)
  add_executable(kaltura-api-client-tests tests/kaltura_api_client_tests.cpp)
  set_target_properties(kaltura-api-client-tests PROPERTIES AUTOMOC OFF)
  target_link_libraries(kaltura-api-client-tests PRIVATE
    kaltura-api-client Qt6::Core Qt6::Network)
  target_compile_definitions(kaltura-api-client-tests PRIVATE
    KALTURA_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME kaltura-api-client-tests COMMAND kaltura-api-client-tests)

  add_executable(kaltura-caption-provider-tests
    tests/caption_provider_tests.cpp
    src/captions/whisper_provider.cpp)
  set_target_properties(kaltura-caption-provider-tests PROPERTIES AUTOMOC OFF)
  target_include_directories(kaltura-caption-provider-tests PRIVATE include)
  target_link_libraries(kaltura-caption-provider-tests PRIVATE whisper)
  add_test(NAME kaltura-caption-provider-tests COMMAND kaltura-caption-provider-tests)

  add_executable(kaltura-cea608-caption-tests
    tests/cea608_caption_inserter_tests.cpp
    src/captions/cea608_caption_inserter.cpp)
  set_target_properties(kaltura-cea608-caption-tests PROPERTIES AUTOMOC OFF)
  target_include_directories(kaltura-cea608-caption-tests PRIVATE
    include "${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs")
  target_link_libraries(kaltura-cea608-caption-tests PRIVATE Qt6::Core)
  add_test(NAME kaltura-cea608-caption-tests COMMAND kaltura-cea608-caption-tests)

  add_executable(kaltura-stream-output-tests
    tests/stream_output_config_tests.cpp
    src/kaltura_player_embed.cpp
    src/stream_output_config.cpp)
  set_target_properties(kaltura-stream-output-tests PROPERTIES AUTOMOC OFF)
  target_include_directories(kaltura-stream-output-tests PRIVATE include)
  target_link_libraries(kaltura-stream-output-tests PRIVATE Qt6::Core)
  add_test(NAME kaltura-stream-output-tests COMMAND kaltura-stream-output-tests)
endif()

set(CPACK_PACKAGE_NAME "kaltura-live")
set(CPACK_PACKAGE_VENDOR "Kaltura Live Plugin Contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Kaltura Live streaming and local captions for OBS Studio")
set(CPACK_PACKAGE_CONTACT "Kaltura Live Plugin Maintainers")
set(CPACK_STRIP_FILES ON)
kaltura_configure_cpack()
include(CPack)
