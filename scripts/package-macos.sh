#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "${project_dir}/VERSION")"
build_dir="${BUILD_DIR:-${project_dir}/build-release-macos}"
dist_dir="${DIST_DIR:-${project_dir}/dist}"
if [[ -n "${QT_PREFIX:-}" ]]; then
  qt_prefix="${QT_PREFIX}"
elif command -v brew >/dev/null 2>&1; then
  qt_prefix=""
  for formula in qt qtbase qt@6; do
    candidate="$(brew --prefix "${formula}" 2>/dev/null || true)"
    if [[ -x "${candidate}/bin/qtpaths" || -x "${candidate}/bin/qtpaths6" ]]; then
      qt_prefix="${candidate}"
      break
    fi
  done
else
  echo "error: set QT_PREFIX to the Qt 6 installation used by OBS" >&2
  exit 1
fi
qtpaths=""
for candidate in "${qt_prefix}/bin/qtpaths" "${qt_prefix}/bin/qtpaths6"; do
  if [[ -x "${candidate}" ]]; then
    qtpaths="${candidate}"
    break
  fi
done
if [[ -z "${qtpaths}" ]]; then
  echo "error: no qtpaths executable found below QT_PREFIX=${qt_prefix}" >&2
  exit 1
fi
macos_architectures="${MACOS_ARCHITECTURES:-$(uname -m)}"
cmake_prefix_path="${qt_prefix}"
if command -v brew >/dev/null 2>&1; then
  simde_prefix="$(brew --prefix simde 2>/dev/null || true)"
  if [[ -d "${simde_prefix}/include" ]]; then
    cmake_prefix_path="${cmake_prefix_path};${simde_prefix}"
  fi
fi
model_dir="${KALTURA_LIVE_MODEL_SOURCE_DIR:-${build_dir}/release-models}"
skip_models=false
if [[ "${1:-}" == "--without-models" ]]; then
  skip_models=true
fi

obs_source="${KALTURA_LIVE_OBS_SOURCE_PATH:-${project_dir}/third_party/obs-studio}"
if [[ ! -f "${obs_source}/libobs/obs-module.h" ]]; then
  git clone --depth 1 --branch "${OBS_VERSION:-32.1.2}" \
    https://github.com/obsproject/obs-studio.git "${obs_source}"
fi
cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="${macos_architectures}" \
  -DCMAKE_PREFIX_PATH="${cmake_prefix_path}" \
  -DKALTURA_LIVE_OBS_SOURCE_PATH="${obs_source}" \
  -DKALTURA_LIVE_OBS_APP_FRAMEWORK_PATH=/Applications/OBS.app/Contents/Frameworks
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure

binary="${build_dir}/libkaltura-live.so"
"${project_dir}/scripts/validate-architecture.sh" macos "${binary}" \
  "${macos_architectures}"

qt_plugin_dir="$(${qtpaths} --plugin-dir)"
qt_version="$(${qtpaths} --qt-version)"
obs_qt_version="$(plutil -extract CFBundleVersion raw \
  /Applications/OBS.app/Contents/Frameworks/QtCore.framework/Versions/A/Resources/Info.plist)"
if [[ "${qt_version}" != "${obs_qt_version}" ]]; then
  echo "error: Qt development version ${qt_version} does not match OBS Qt ${obs_qt_version}" >&2
  exit 1
fi
tls_backend="${qt_plugin_dir}/tls/libqsecuretransportbackend.dylib"
if [[ ! -f "${tls_backend}" ]]; then
  echo "error: Secure Transport backend not found at ${tls_backend}" >&2
  exit 1
fi

if [[ "${skip_models}" == false ]]; then
  "${project_dir}/scripts/download-whisper-models.sh" "${model_dir}"
fi

staging_dir="$(mktemp -d /tmp/kaltura-live-package.XXXXXX)"
trap 'rm -rf "${staging_dir}"' EXIT
bundle="${staging_dir}/root/Library/Application Support/obs-studio/plugins/kaltura-live.plugin"
mkdir -p "${bundle}/Contents/MacOS" "${bundle}/Contents/PlugIns/tls" \
  "${bundle}/Contents/Resources/models" "${dist_dir}"
cp "${binary}" "${bundle}/Contents/MacOS/kaltura-live"
cp "${tls_backend}" "${bundle}/Contents/PlugIns/tls/libqsecuretransportbackend.dylib"
sed "s/@VERSION@/${version}/g" "${project_dir}/packaging/macos/Info.plist.in" \
  > "${bundle}/Contents/Info.plist"
if [[ "${skip_models}" == false ]]; then
  cp "${model_dir}/ggml-tiny.en.bin" "${model_dir}/ggml-base.en.bin" \
    "${bundle}/Contents/Resources/models/"
fi
/usr/bin/strip -x "${bundle}/Contents/MacOS/kaltura-live"

obs_rpath='@executable_path/../Frameworks'
for target in "${bundle}/Contents/MacOS/kaltura-live" \
  "${bundle}/Contents/PlugIns/tls/libqsecuretransportbackend.dylib"; do
  if ! otool -l "${target}" | grep -F "${obs_rpath}" >/dev/null; then
    install_name_tool -add_rpath "${obs_rpath}" "${target}"
  fi
done

application_identity="${MACOS_APPLICATION_SIGNING_IDENTITY:--}"
codesign --force --deep --sign "${application_identity}" --timestamp=none "${bundle}"
xattr -cr "${bundle}"
codesign --verify --deep --strict "${bundle}"
xattr -cr "${staging_dir}/root"

artifact_arch="${macos_architectures//;/_}"
archive="${dist_dir}/kaltura-live-${version}-macOS-${artifact_arch}.tar.gz"
COPYFILE_DISABLE=1 tar -C "$(dirname "${bundle}")" -czf "${archive}" "$(basename "${bundle}")"

unsigned_pkg="${staging_dir}/kaltura-live-unsigned.pkg"
scripts_dir="${staging_dir}/scripts"
mkdir -p "${scripts_dir}"
cp "${project_dir}/packaging/macos/postinstall" "${scripts_dir}/postinstall"
chmod 755 "${scripts_dir}/postinstall"
COPYFILE_DISABLE=1 pkgbuild --root "${staging_dir}/root" \
  --scripts "${scripts_dir}" \
  --filter '(^|/)\._.*' \
  --identifier com.kaltura.obs.kaltura-live \
  --version "${version}" \
  --install-location / \
  "${unsigned_pkg}"
package="${dist_dir}/kaltura-live-${version}-macOS-${artifact_arch}.pkg"
if [[ -n "${MACOS_INSTALLER_SIGNING_IDENTITY:-}" ]]; then
  productsign --sign "${MACOS_INSTALLER_SIGNING_IDENTITY}" "${unsigned_pkg}" "${package}"
else
  cp "${unsigned_pkg}" "${package}"
fi

if [[ "${MACOS_NOTARIZE:-false}" == true ]]; then
  : "${APPLE_NOTARY_PROFILE:?APPLE_NOTARY_PROFILE is required when MACOS_NOTARIZE=true}"
  xcrun notarytool submit "${package}" --keychain-profile "${APPLE_NOTARY_PROFILE}" --wait
  xcrun stapler staple "${package}"
fi

(cd "${dist_dir}" && \
  shasum -a 256 "$(basename "${archive}")" "$(basename "${package}")" \
    > SHA256SUMS-macos.txt)
echo "Created ${archive}"
echo "Created ${package}"
