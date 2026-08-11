include(FetchContent)

set(KALTURA_LIVE_OBS_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-studio" CACHE PATH
  "OBS Studio source tree (headers and libcaption)")
set(KALTURA_LIVE_MODEL_SOURCE_DIR "" CACHE PATH
  "Optional directory containing ggml-tiny.en.bin and ggml-base.en.bin")
set(KALTURA_LIVE_OBS_SDK_PATH "" CACHE PATH
  "Optional OBS SDK/build prefix used when OBS CMake packages are unavailable")
set(KALTURA_LIVE_LIBOBS_LIBRARY "" CACHE FILEPATH "Path to the libobs link library")
set(KALTURA_LIVE_OBS_FRONTEND_LIBRARY "" CACHE FILEPATH
  "Path to the obs-frontend-api link library")

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)
find_package(libobs QUIET CONFIG)
find_package(obs-frontend-api QUIET CONFIG)

if(NOT EXISTS "${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs/obs-module.h")
  message(FATAL_ERROR
    "OBS source headers were not found at KALTURA_LIVE_OBS_SOURCE_PATH='${KALTURA_LIVE_OBS_SOURCE_PATH}'. "
    "Clone the supported OBS source tag there or set this cache variable explicitly.")
endif()
if(NOT EXISTS "${KALTURA_LIVE_OBS_SOURCE_PATH}/deps/libcaption/CMakeLists.txt")
  message(FATAL_ERROR "The selected OBS source tree does not contain deps/libcaption")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(whisper_cpp
  GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
  GIT_TAG v1.9.1
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(whisper_cpp)
add_subdirectory("${KALTURA_LIVE_OBS_SOURCE_PATH}/deps/libcaption"
  "${CMAKE_CURRENT_BINARY_DIR}/_deps/obs-libcaption-build" EXCLUDE_FROM_ALL)

function(kaltura_configure_obs_target target)
  if(TARGET OBS::libobs AND TARGET OBS::obs-frontend-api)
    target_link_libraries(${target} PRIVATE OBS::libobs OBS::obs-frontend-api)
    return()
  endif()

  set(obs_include_hints
    "${KALTURA_LIVE_OBS_SDK_PATH}/include"
    "${KALTURA_LIVE_OBS_SOURCE_PATH}/libobs")
  set(frontend_include_hints
    "${KALTURA_LIVE_OBS_SDK_PATH}/include"
    "${KALTURA_LIVE_OBS_SOURCE_PATH}/frontend/api")

  if(APPLE)
    kaltura_link_obs_fallback(${target})
    return()
  endif()

  find_path(KALTURA_LIVE_LIBOBS_INCLUDE_DIR obs-module.h HINTS ${obs_include_hints})
  find_path(KALTURA_LIVE_OBS_FRONTEND_INCLUDE_DIR obs-frontend-api.h
    HINTS ${frontend_include_hints})
  if(NOT KALTURA_LIVE_LIBOBS_LIBRARY)
    find_library(KALTURA_LIVE_LIBOBS_LIBRARY NAMES obs libobs
      HINTS "${KALTURA_LIVE_OBS_SDK_PATH}/lib" "${KALTURA_LIVE_OBS_SDK_PATH}/bin/64bit")
  endif()
  if(NOT KALTURA_LIVE_OBS_FRONTEND_LIBRARY)
    find_library(KALTURA_LIVE_OBS_FRONTEND_LIBRARY NAMES obs-frontend-api
      HINTS "${KALTURA_LIVE_OBS_SDK_PATH}/lib" "${KALTURA_LIVE_OBS_SDK_PATH}/bin/64bit")
  endif()
  if(NOT KALTURA_LIVE_LIBOBS_INCLUDE_DIR OR NOT KALTURA_LIVE_OBS_FRONTEND_INCLUDE_DIR OR
     NOT KALTURA_LIVE_LIBOBS_LIBRARY OR NOT KALTURA_LIVE_OBS_FRONTEND_LIBRARY)
    message(FATAL_ERROR
      "Could not locate the OBS SDK. Install libobs/obs-frontend-api CMake packages, or set "
      "KALTURA_LIVE_OBS_SDK_PATH, KALTURA_LIVE_LIBOBS_LIBRARY, and "
      "KALTURA_LIVE_OBS_FRONTEND_LIBRARY.")
  endif()
  target_include_directories(${target} PRIVATE
    "${KALTURA_LIVE_LIBOBS_INCLUDE_DIR}" "${KALTURA_LIVE_OBS_FRONTEND_INCLUDE_DIR}")
  target_link_libraries(${target} PRIVATE
    "${KALTURA_LIVE_LIBOBS_LIBRARY}" "${KALTURA_LIVE_OBS_FRONTEND_LIBRARY}")
endfunction()
