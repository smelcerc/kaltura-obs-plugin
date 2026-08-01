#!/usr/bin/env bash
set -euo pipefail

if [[ "${CI:-}" != true || -z "${RUNNER_TEMP:-}" || -z "${GITHUB_ENV:-}" ]]; then
  echo "error: setup-macos-ci.sh may only run on an ephemeral GitHub Actions runner" >&2
  exit 1
fi

obs_version="${OBS_VERSION:-32.2.1}"
obs_arch="${OBS_MACOS_ARCH:-Intel}"
if [[ "${obs_version}" != 32.2.1 || "${obs_arch}" != Intel ]]; then
  echo "error: no verified OBS checksum is configured for ${obs_version} ${obs_arch}" >&2
  exit 1
fi

obs_sha256="6900a7a6d4422956114cac3c148d871307fdb0530160c2cbb4e97f624c9f85a5"
obs_dmg="${RUNNER_TEMP}/OBS-Studio-${obs_version}-macOS-${obs_arch}.dmg"
obs_mount="${RUNNER_TEMP}/obs-mount"

curl --fail --location --retry 3 --proto '=https' \
  "https://github.com/obsproject/obs-studio/releases/download/${obs_version}/OBS-Studio-${obs_version}-macOS-${obs_arch}.dmg" \
  --output "${obs_dmg}"
printf '%s  %s\n' "${obs_sha256}" "${obs_dmg}" | shasum -a 256 --check

mkdir -p "${obs_mount}"
hdiutil attach "${obs_dmg}" -nobrowse -quiet -mountpoint "${obs_mount}"
trap 'hdiutil detach "${obs_mount}" -quiet >/dev/null 2>&1 || true' EXIT
rm -rf /Applications/OBS.app
ditto "${obs_mount}/OBS.app" /Applications/OBS.app
hdiutil detach "${obs_mount}" -quiet
trap - EXIT

obs_qt_version="$(plutil -extract CFBundleVersion raw \
  /Applications/OBS.app/Contents/Frameworks/QtCore.framework/Versions/A/Resources/Info.plist)"
aqt_venv="${RUNNER_TEMP}/aqt-venv"
qt_root="${RUNNER_TEMP}/Qt"
python3 -m venv "${aqt_venv}"
"${aqt_venv}/bin/python" -m pip install --disable-pip-version-check aqtinstall
"${aqt_venv}/bin/aqt" install-qt mac desktop "${obs_qt_version}" clang_64 \
  --archives qtbase --outputdir "${qt_root}"

qtpaths="$(find "${qt_root}" -type f -path '*/bin/qtpaths' -print -quit)"
if [[ -z "${qtpaths}" ]]; then
  echo "error: qtpaths was not installed by aqtinstall" >&2
  exit 1
fi
qt_prefix="$(dirname "$(dirname "${qtpaths}")")"
if [[ "$("${qtpaths}" --qt-version)" != "${obs_qt_version}" ]]; then
  echo "error: installed Qt does not match the OBS runtime" >&2
  exit 1
fi
printf 'QT_PREFIX=%s\n' "${qt_prefix}" >> "${GITHUB_ENV}"
