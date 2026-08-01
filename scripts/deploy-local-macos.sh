#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
QT_PREFIX="${QT_PREFIX:-/usr/local/opt/qtbase}"
OBS_HEADERS_DIR="${OBS_HEADERS_DIR:-${ROOT_DIR}/third_party/obs-studio}"
OBS_APP_DIR="${OBS_APP_DIR:-/Applications/OBS.app}"
OBS_FRAMEWORKS_DIR="${OBS_APP_DIR}/Contents/Frameworks"
OBS_EXECUTABLE="${OBS_APP_DIR}/Contents/MacOS/OBS"
PLUGIN_BUNDLE_DIR="${PLUGIN_BUNDLE_DIR:-${HOME}/Library/Application Support/obs-studio/plugins/kaltura-live.plugin}"
PLUGIN_BINARY_PATH="${PLUGIN_BUNDLE_DIR}/Contents/MacOS/kaltura-live"
PLUGIN_INFO_PLIST_PATH="${PLUGIN_BUNDLE_DIR}/Contents/Info.plist"
PLUGIN_TLS_DIR="${PLUGIN_BUNDLE_DIR}/Contents/PlugIns/tls"
PLUGIN_MODELS_DIR="${PLUGIN_BUNDLE_DIR}/Contents/Resources/models"
PLUGIN_VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"
QT_PLUGIN_DIR="$(${QT_PREFIX}/bin/qtpaths --plugin-dir)"
QT_TLS_BACKEND_PATH="${QT_PLUGIN_DIR}/tls/libqsecuretransportbackend.dylib"
JOBS="${JOBS:-4}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "error: cmake is not installed"
  exit 1
fi

if [ ! -f "${OBS_HEADERS_DIR}/libobs/obs-module.h" ]; then
  echo "obs headers not found, cloning obsproject/obs-studio..."
  git clone --depth 1 https://github.com/obsproject/obs-studio.git "${OBS_HEADERS_DIR}"
fi

if [ ! -x "${OBS_FRAMEWORKS_DIR}/QtCore.framework/Versions/A/QtCore" ]; then
  echo "error: OBS Qt frameworks not found in ${OBS_FRAMEWORKS_DIR}"
  exit 1
fi

if [ ! -x "${OBS_EXECUTABLE}" ]; then
  echo "error: OBS executable not found at ${OBS_EXECUTABLE}"
  exit 1
fi

if [ ! -f "${QT_TLS_BACKEND_PATH}" ]; then
  echo "error: Qt Secure Transport backend not found at ${QT_TLS_BACKEND_PATH}"
  exit 1
fi

QT_VERSION="$(${QT_PREFIX}/bin/qtpaths --qt-version)"
OBS_QT_VERSION="$(plutil -extract CFBundleVersion raw \
  "${OBS_FRAMEWORKS_DIR}/QtCore.framework/Versions/A/Resources/Info.plist")"
if [ "${QT_VERSION}" != "${OBS_QT_VERSION}" ]; then
  echo "error: Qt headers are ${QT_VERSION}, but OBS bundles Qt ${OBS_QT_VERSION}"
  echo "set QT_PREFIX to a matching Qt development installation"
  exit 1
fi

echo "configuring build..."
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_PREFIX_PATH="${QT_PREFIX}" \
  -DKALTURA_LIVE_OBS_APP_FRAMEWORK_PATH="${OBS_FRAMEWORKS_DIR}"

echo "building plugin..."
cmake --build "${BUILD_DIR}" -j"${JOBS}"

if [ ! -f "${BUILD_DIR}/libkaltura-live.so" ]; then
  echo "error: build succeeded but plugin binary was not found at ${BUILD_DIR}/libkaltura-live.so"
  exit 1
fi

echo "deploying to local OBS plugin path..."
mkdir -p "$(dirname "${PLUGIN_BINARY_PATH}")"
cp -f "${BUILD_DIR}/libkaltura-live.so" "${PLUGIN_BINARY_PATH}"

mkdir -p "${PLUGIN_TLS_DIR}"
cp -f "${QT_TLS_BACKEND_PATH}" "${PLUGIN_TLS_DIR}/libqsecuretransportbackend.dylib"

"${ROOT_DIR}/scripts/download-whisper-models.sh" "${PLUGIN_MODELS_DIR}"

TLS_BACKEND_BINARY="${PLUGIN_TLS_DIR}/libqsecuretransportbackend.dylib"
OBS_RELATIVE_FRAMEWORK_RPATH='@executable_path/../Frameworks'

if otool -l "${TLS_BACKEND_BINARY}" | grep -F '@loader_path/../../../../lib' >/dev/null; then
  install_name_tool -rpath '@loader_path/../../../../lib' \
    "${OBS_RELATIVE_FRAMEWORK_RPATH}" "${TLS_BACKEND_BINARY}"
elif ! otool -l "${TLS_BACKEND_BINARY}" | grep -F "${OBS_RELATIVE_FRAMEWORK_RPATH}" >/dev/null; then
  install_name_tool -add_rpath "${OBS_RELATIVE_FRAMEWORK_RPATH}" "${TLS_BACKEND_BINARY}"
fi

if otool -l "${PLUGIN_BINARY_PATH}" | grep -F "${OBS_FRAMEWORKS_DIR}" >/dev/null; then
  install_name_tool -rpath "${OBS_FRAMEWORKS_DIR}" \
    "${OBS_RELATIVE_FRAMEWORK_RPATH}" "${PLUGIN_BINARY_PATH}"
elif ! otool -l "${PLUGIN_BINARY_PATH}" | grep -F "${OBS_RELATIVE_FRAMEWORK_RPATH}" >/dev/null; then
  install_name_tool -add_rpath "${OBS_RELATIVE_FRAMEWORK_RPATH}" "${PLUGIN_BINARY_PATH}"
fi

if otool -L "${PLUGIN_BINARY_PATH}" | grep -E '/(usr/local|opt/homebrew)/.*Qt(Core|Gui|Network|Widgets)' >/dev/null; then
  echo "error: plugin unexpectedly links to Qt outside OBS.app"
  otool -L "${PLUGIN_BINARY_PATH}"
  exit 1
fi

if otool -L "${TLS_BACKEND_BINARY}" | \
    grep -E '/(usr/local|opt/homebrew)/.*Qt(Core|Network)' >/dev/null; then
  echo "error: TLS backend unexpectedly links to Qt outside OBS.app"
  otool -L "${TLS_BACKEND_BINARY}"
  exit 1
fi

OBS_ARCHS="$(lipo -archs "${OBS_EXECUTABLE}")"
for architecture in ${OBS_ARCHS}; do
  if ! lipo "${PLUGIN_BINARY_PATH}" -verify_arch "${architecture}" >/dev/null 2>&1; then
    echo "error: plugin does not contain OBS architecture ${architecture}"
    exit 1
  fi
  if ! lipo "${TLS_BACKEND_BINARY}" -verify_arch "${architecture}" >/dev/null 2>&1; then
    echo "error: TLS backend does not contain OBS architecture ${architecture}"
    exit 1
  fi
done

if otool -l "${PLUGIN_BINARY_PATH}" "${TLS_BACKEND_BINARY}" | \
    grep -F "${OBS_APP_DIR}" >/dev/null; then
  echo "error: deployed binaries contain an absolute OBS.app runtime path"
  exit 1
fi

sed "s/@VERSION@/${PLUGIN_VERSION}/g" \
  "${ROOT_DIR}/packaging/macos/Info.plist.in" > "${PLUGIN_INFO_PLIST_PATH}"

# install_name_tool invalidates signatures. Ad-hoc sign the completed local
# bundle so macOS can validate every nested binary consistently.
codesign --force --deep --sign - --timestamp=none "${PLUGIN_BUNDLE_DIR}"
codesign --verify --deep --strict "${PLUGIN_BUNDLE_DIR}"

echo "done"
echo "installed: ${PLUGIN_BINARY_PATH}"
